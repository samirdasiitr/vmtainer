# vmtainer: Design Document

## 1. Overview

vmtainer is a minimal KVM-based micro-VM runtime that provides container-like
isolation using hardware virtualization. Each container runs inside its own
lightweight VM with a custom Linux 6.10 kernel, sharing its filesystem via
virtiofs and networking via virtio-net/TAP.

The key innovation is **snapshot-restore architecture**: a golden snapshot is
taken at a deterministic boot point (after kernel init, before workload
execution), and all subsequent container instances are restored from this
snapshot rather than booting from scratch. This reduces container startup from
~800ms (full boot) to **~13ms** (warm cache restore).

### 1.1 Goals

- **Sub-20ms container cold-start** from snapshot restore
- **Hardware-level isolation** (KVM) with container-like UX
- **Minimal attack surface** (~2700 LOC single-file VMM, no QEMU)
- **OCI image compatibility** via virtiofs rootfs sharing
- **Kubernetes integration** via CRI plugin

### 1.2 Non-Goals

- Multi-CPU guest support (single vCPU only)
- GPU/device passthrough
- Live migration (offline snapshots only)
- Windows guest support

## 2. Architecture

```
                    +-----------------------+
                    |    Kubernetes / CLI    |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    |     vmtainer CRI      |  (gRPC, Go)
                    |   RuntimeService +    |
                    |   ImageService        |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    |     vmtainer VMM      |  (C++, ~2700 LOC)
                    |                       |
                    |  +---------+  +-----+ |
                    |  |   KVM   |  | TAP | |
                    |  +---------+  +-----+ |
                    |  |  vCPU   |  |     | |
                    |  |  RAM    |  | eth0| |
                    |  +---------+  +-----+ |
                    |       |               |
                    |  +----v----+          |
                    |  |virtiofs |          |
                    |  +---------+          |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    |   virtiofsd process   |
                    |   (shares OCI rootfs) |
                    +-----------------------+
```

### 2.1 Component Summary

| Component | Language | LOC | Role |
|-----------|----------|-----|------|
| VMM (`vmm.cpp`) | C++17 | ~2700 | KVM setup, MMIO virtio, snapshot save/restore |
| Guest init (`init`) | Shell | ~120 | Mount virtiofs, configure network, chroot, run entrypoint |
| Guest kernel | C (Linux 6.10) | - | Minimal config: serial, virtio-mmio, virtiofs, virtio-net |
| CRI plugin | Go | ~2900 | Kubernetes RuntimeService + ImageService |
| Kernel module | C | ~250 | vmalloc-backed snapshot for zero-miss restore |
| Clone script | Bash | ~130 | Pull OCI image, extract rootfs, restore VM |

### 2.2 VMM Internals

The VMM is a single C++ file implementing:

1. **KVM lifecycle**: `/dev/kvm` -> VM fd -> vCPU fd -> memslot -> run loop
2. **Boot**: Load bzImage + initrd via Linux boot protocol, set up e820 map,
   GDT, page tables, serial (0x3f8), PIT, LAPIC, IOAPIC
3. **MMIO virtio transport**: Two virtio-mmio devices:
   - `0xd0001000`: virtiofs (type 26) with vhost-user backend
   - `0xd0002000`: virtio-net (type 1) with userspace data path
4. **Snapshot signal**: Guest writes magic `0x48594C54` to `0xd0000000`;
   KVM traps (no memslot), VMM saves full state
5. **Virtio-net**: Userspace packet forwarding between virtqueue and TAP fd
   (dedicated pthread with poll loop)

### 2.3 Memory Layout

```
Guest Physical Address Map:

0x00000000 - 0x0009FBFF  Conventional memory (639 KB)
0x0009FC00 - 0x0009FFFF  Reserved
0x000F0000 - 0x000FFFFF  BIOS ROM area (custom BIOS)
0x00100000 - 0x03FFFFFF  Usable RAM (63 MB, kernel + initrd)
0xD0000000              Snapshot signal MMIO
0xD0001000 - 0xD0001FFF  Virtio-fs MMIO (IRQ 5)
0xD0002000 - 0xD0002FFF  Virtio-net MMIO (IRQ 6)
```

RAM is backed by a memfd (anonymous shared memory), allowing zero-copy mmap
into userspace and efficient snapshot restore via sparse copy.

## 3. Container Image Execution via virtiofs & Entrypoint

vmtainer provides standard OCI/Docker container image compatibility without
running a Docker daemon or container runtime inside the micro-VM. Standard
container images are unpacked on the host, exposed to the guest VM through a
dedicated `virtiofsd` daemon, configured via metadata injection files
(`.entrypoint` and `.vmconfig`), and executed inside an isolated `chroot`
environment.

### 3.1 Docker / OCI Image Extraction

Container images are pulled and flattened on the host to create a clean root
filesystem (`rootfs`) for each container:

1. **Pull & Create**: The host runtime (CLI script or CRI plugin) fetches the image using standard tooling:
   - **CLI (`clone.sh`)**: Runs `docker pull <image>`, then `docker create <image> /bin/true` to create a stopped container instance and extract its image layers and metadata.
   - **CRI Plugin (`ImageService.PullImage`)**: Pulls the image and persists canonical digests, tags, and layer information in `/var/lib/vmtainer/images/<digest>/`.
2. **Filesystem Extraction**: The container filesystem is flattened into a host directory:
   ```bash
   docker export "$CONTAINER_ID" | tar -xf - -C "$ROOTFS_DIR"
   ```
   For CRI containers, `CreateContainer` copies or bind-mounts the cached rootfs into `/var/lib/vmtainer/containers/<container-id>/rootfs`, ensuring each container instance has its own isolated, writable filesystem tree.
3. **Metadata Inspection**: The default entrypoint and command arguments are parsed from the image manifest (`Config.Entrypoint` and `Config.Cmd` via `docker inspect`, or CRI `Command` and `Args` specifications). If unspecified, it defaults to `/bin/sh`.

### 3.2 Exposing Rootfs via virtiofs & vhost-user

Rather than attaching heavy block devices or formatting virtual disk images,
vmtainer uses **virtiofs** (virtio-fs) to share the host directory directly with
the guest at near-native filesystem speed:

```
+--------------------------------------------------------------------+
| HOST                                                               |
|                                                                    |
|  Container Rootfs: /var/lib/vmtainer/containers/<id>/rootfs/       |
|  +-- bin/, etc/, usr/, ...                                         |
|  +-- .entrypoint (injected by host)                                |
|  +-- .vmconfig   (injected by host)                                |
|        ^                                                           |
|        | (directory passthrough)                                   |
|  +-----+------------+                                              |
|  |    virtiofsd     | (Rust daemon, --sandbox none --cache never)  |
|  +-----+------------+                                              |
|        | Unix socket: /tmp/vmtainer-vhost-<pid>.sock               |
|        v (vhost-user protocol)                                     |
|  +-----+------------+        KVM memfd                             |
|  |   vmtainer VMM   | <======================+                     |
|  +------------------+                        |                     |
+--------|-------------------------------------|---------------------+
         | virtio-mmio (0xd0001000, IRQ 5)      | (shared guest RAM)
+--------v-------------------------------------v---------------------+
| GUEST MICRO-VM                                                     |
|                                                                    |
|  Linux Kernel (virtio_fs + virtio_mmio driver)                     |
|        |                                                           |
|  initrd: /init                                                     |
|        | mount -t virtiofs myfs /share                             |
|        v                                                           |
|  /share (mapped to Host Container Rootfs)                          |
|        |                                                           |
|        +--> chroot /share <ENTRYPOINT>                             |
+--------------------------------------------------------------------+
```

1. **Dedicated virtiofsd per VM**: For each restored micro-VM, the VMM forks an instance of the Rust-based `virtiofsd` daemon:
   ```bash
   /usr/libexec/virtiofsd \
       --socket-path /tmp/vmtainer-vhost-<pid>.sock \
       --shared-dir <rootfs_dir> \
       --sandbox none \
       --cache never
   ```
2. **vhost-user IPC**: The VMM connects to the daemon's Unix domain socket and performs the vhost-user protocol handshake.
3. **Zero-Copy Memory Sharing**: The guest RAM backing descriptor (`memfd`) is passed to `virtiofsd` across the Unix domain socket. This allows `virtiofsd` to directly read requests from guest virtqueues and write file data into guest physical memory without VMM bounce buffering.
4. **MMIO Virtio Transport**: The VMM exposes virtiofs as a standard virtio-mmio device at base address `0xd0001000` (IRQ 5, Device ID 26).

### 3.3 Configuration Injection (`.entrypoint` and `.vmconfig`)

To keep guest startup fast and eliminate the need for heavy guest agents or extra virtual configuration devices, the host VMM injects configuration files directly into the root of the shared directory before launching the VM:

1. **`.entrypoint`**: Contains the command string to be executed inside the container:
   ```
   /bin/sh -c "python3 app.py"
   ```
   When configured via CRI or CLI, working directory prefixes are encoded directly (e.g., `cd /app && python3 app.py`).
2. **`.vmconfig`**: A key-value shell file containing container metadata, networking parameters, and environment variables:
   ```bash
   HOSTNAME=web-backend-0
   NET_IP=10.0.0.2/24
   NET_GW=10.0.0.1
   NET_MAC=52:54:00:12:34:56
   ENV_PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
   ENV_PORT=8080
   ENV_NODE_ENV=production
   ```

### 3.4 Snapshot Decoupling & Clean FUSE Handshake

The snapshot-restore architecture requires careful coordination with virtiofs:

- **The FUSE Session Dilemma**: A live virtiofs mount involves stateful FUSE session negotiation (`FUSE_INIT`, unique session tokens, open file handles, and daemon queues). If a snapshot were taken *after* mounting virtiofs, restoring into a new `virtiofsd` process with a different root directory would cause stale session handles and I/O panics.
- **Deterministic Signal Timing**: In vmtainer, the golden snapshot is taken in the guest `init` script **immediately after** kernel drivers are initialized, but **before** `mount -t virtiofs` is called.
- **Clean Restores**: When `vmtainer restore` is invoked, the VMM launches a fresh `virtiofsd` daemon pointed at the new container's rootfs. When the vCPU resumes execution from the snapshot, the guest `init` executes `mount -t virtiofs myfs /share` against the newly initialized `virtiofsd`, establishing a clean, valid FUSE session every time.

### 3.5 Guest Init Sequence: Mount, Chroot, and Execution

Once restored, the guest `init` process (`PID 1` inside the initrd) executes the container lifecycle:

1. **Mount virtiofs**:
   ```sh
   mount -t virtiofs myfs /share
   ```
2. **Load Configuration**:
   - Reads the entrypoint command from `/share/.entrypoint`.
   - Sources network, hostname, and environment parameters from `/share/.vmconfig`.
3. **Configure Network & Host**:
   - Sets the hostname via `/proc/sys/kernel/hostname`.
   - Brings up `eth0`, assigns `$NET_IP`, and adds the default gateway `$NET_GW`.
4. **Pseudo-Filesystem Preparation**:
   Prepares virtual filesystems inside the container rootfs:
   ```sh
   mkdir -p /share/proc /share/sys /share/dev /share/tmp
   mount --move /proc /share/proc || mount -t proc proc /share/proc
   mount --move /sys  /share/sys  || mount -t sysfs sysfs /share/sys
   mount --move /dev  /share/dev  || mount -t devtmpfs devtmpfs /share/dev
   [ -e /share/dev/kmsg ] || mknod /share/dev/kmsg c 1 11
   ```
5. **Environment Export**:
   Iterates through all variables matching `ENV_*` in `.vmconfig` and exports them (e.g., `ENV_PORT=8080` becomes `export PORT=8080`).
6. **Chroot and Entrypoint Execution**:
   Switches into the container filesystem and executes the target entrypoint:
   ```sh
   chroot /share $ENTRYPOINT
   RC=$?
   ```
7. **Clean VM Halt**:
   When the entrypoint process exits, `init` logs `entrypoint exited ($RC)` to `/dev/kmsg` and halts the machine:
   ```sh
   exec /bin/halt -f
   ```
   The kernel halts, KVM traps VM shutdown, and the VMM collects the container exit code and terminates `virtiofsd`.

## 4. Snapshot Format

### 4.1 Header (v7)

The snapshot is a single flat file containing all KVM state needed to restore
a vCPU + VM to its exact pre-snapshot state.

```
Offset   Size    Field
------   ----    -----
0        8       Magic (0x48594C54534E4150 = "PANSTLYH")
8        4       Version (7)
12       4       RAM size in MB
16       ...     kvm_regs, kvm_sregs, kvm_lapic_state,
                 kvm_vcpu_events, kvm_xcrs, kvm_mp_state,
                 kvm_debugregs, kvm_clock_data, kvm_pit_state2,
                 kvm_irqchip (PIC master/slave/IOAPIC)
3812     4       TSC KHz
3812     4       CPUID entry count
3816     4       MSR count
3820     4       XSAVE buffer size
3824     ...     Virtio-fs device state (status, features, VQ state)
3952     ...     Virtio-net device state (status, features, VQ state, MAC)
3992     4       Dirty bitmap size in bytes
3996     4       Padding

---- Variable-length sections (immediately after header) ----

4000     xsave_size      XSAVE buffer
+offset  cpuid_nent*40   CPUID entries (kvm_cpuid_entry2)
+offset  num_msrs*16     MSR entries (kvm_msr_entry)
+offset  bitmap_bytes    Dirty page bitmap (1 bit per 4KB page)
+offset  ram_mb*1M       Guest RAM contents

Total for default config: ~65 MB (4000B header + 8192B xsave +
  2080B cpuid + 1264B msrs + 2048B bitmap + 64MB RAM)
```

### 4.2 Dirty Page Bitmap

On snapshot save, every 4KB page of guest RAM is scanned with a fast
64-bit OR-reduce. Non-zero pages are marked in a bitmap (1 bit per page).
For 64MB RAM, the bitmap is 2048 bytes (16384 pages).

Typical dirty ratio: **~46%** (7458/16384 pages = 29MB of 64MB).

On restore, only dirty pages are copied, saving both time (skip zero-page
reads) and physical memory (memfd pages stay zero until written).

## 5. Snapshot Restore Pipeline

```
restore_snapshot_file(path):
  1. mmap(snapshot_file, PROT_READ, MAP_PRIVATE)     [0 ms - lazy]
  2. parse header, xsave, cpuid, msrs, bitmap        [< 0.1 ms]
  3. init KVM (VM fd, vCPU fd, memfd, memslot)        [0.3-0.8 ms]
  4. start virtiofsd + vhost-user handshake            [1.2-1.6 ms]
  5. open TAP device                                   [0.1 ms]
  6. sparse RAM copy (bitmap-guided)                   [10-14 ms]
     - for each set bit in bitmap:
         memcpy(dst + pg*4096, src + pg*4096, 4096)
  7. restore KVM state (regs, sregs, CPUID, MSRs...)   [< 0.5 ms]
  8. restore virtio device state                        [< 0.1 ms]
  9. start net thread + resume vCPU                     [< 0.1 ms]

Total: ~13 ms (warm cache)
```

## 6. Optimization History

### 6.1 Timeline

| # | Optimization | Before | After | Speedup |
|---|-------------|--------|-------|---------|
| 1 | Baseline (full boot) | 800ms | 150ms | 5.3x (snapshot) |
| 2 | virtiofsd fast backoff (1ms exp) | 100ms wait | 1.5ms | 67x |
| 3 | Skip RAM zeroing on restore | 20ms KVM init | 0.5ms | 40x |
| 4 | mmap snapshot file (no read()) | 27ms | 25ms | 1.1x |
| 5 | Sparse copy (skip zero pages) | 25ms snap | 15ms | 1.7x |
| 6 | Dirty page bitmap in snapshot | 15ms snap | 12ms | 1.25x |
| 7 | Drop MAP_POPULATE (lazy faults) | 12ms snap | 12ms | ~same warm |
| 8 | Multi-thread dynamic coalesced memcpy | 12ms snap | 7.6ms | 1.6x |
| 9 | userfaultfd demand-paged lazy restore | 7.6ms snap | 0.3ms | 25x (2.7ms total) |

### 6.2 Current Breakdown (Single VM, Warm Cache)

#### userfaultfd Demand-Paged Mode (Default)
```
Component           Time (ms)    % of total
---------           ---------    ----------
KVM init            0.6          22%
virtiofsd startup   1.6          59%
TAP connect         0.2          7%
Snapshot RAM setup  0.3          11%
  (uffd registration + ring pre-fault)
KVM state restore   < 0.1        <1%
---------           ---------    ----------
Total VMM restore   ~2.7         100%
```

#### Memcpy Fallback Mode (--no-uffd)
```
Component           Time (ms)    % of total
---------           ---------    ----------
KVM init            0.6-1.0      6%
virtiofsd startup   1.2-1.6      12%
TAP connect         0.2          1%
RAM sparse copy     7.6-9.8      80%
  (29MB dirty pages)
KVM state restore   < 0.1        <1%
---------           ---------    ----------
Total VMM restore   10-12        100%
```

### 6.3 Remaining Bottleneck & Optimization Analysis

With `userfaultfd`, the VMM snapshot restore bottleneck was eliminated, reducing restore time
from 12-17ms down to **2.1-3.4ms**.

The remaining latency is in the **guest initialization phase (virtiofs mount + chroot)**,
which takes ~28ms inside the 1-vCPU guest kernel.

### 6.4 End-to-End Timestamped Clone Execution Trace

Below is a real execution trace of `scripts/clone.sh` launching an Alpine container clone,
with microsecond-precision timestamps relative to the VMM process start (`t0 = 0.00ms`):

```
=== vmtainer clone ===
  Image:     alpine:latest
  Clone ID:  clone-1789027341-85129
  Rootfs:    /tmp/alpine-rootfs

[1/4] Using pre-extracted rootfs (skipped pull & extract)
[3/4] Preparing config...
[4/4] Restoring VM from snapshot...
[+  0.64ms] [VMM] started virtiofsd (pid 85141) sharing /tmp/alpine-rootfs
[+  2.19ms] [VMM] virtiofs connected, tag='myfs'
[+  2.20ms] [VMM] virtio-net: mac=52:54:00:12:34:56 (restore)
[+  2.40ms] [VMM] TAP connected: tap0
[+  2.65ms] [VMM] virtio-net: userspace data path active
[+  2.66ms] [VMM] restored in 2.7ms  kvm_init=0.6ms virtiofsd=1.6ms tap=0.2ms snap=0.3ms (uffd=1)
[+  9.82ms] VMTAINER: mounting virtiofs
[+ 15.66ms] VMTAINER: virtiofs mounted OK
[+ 18.98ms] VMTAINER: reading config
[+ 21.65ms] VMTAINER: hostname=vmtainer
[+ 26.92ms] VMTAINER: chroot into virtiofs, entrypoint=echo hello
[+ 30.76ms] VMTAINER: running entrypoint: echo hello

[VMM] ====================================================
[VMM] TIME TO START OF ENTRYPOINT EXECUTION: 30.77ms
[VMM]   - VMM setup & snapshot restore:        2.67ms
[VMM]   - Guest mount & init to entrypoint:    28.10ms
[VMM] ====================================================

[+ 36.40ms] VMTAINER: entrypoint exited (0)
hello
reboot: System halted

=== Clone complete ===
  Pull+extract:       0ms
  Snapshot restore:   2.7ms
  Time to entrypoint: 30.77ms
  Total VM runtime:   99ms
  Total E2E time:     99ms
  Exit code:          0
```

## 7. Kernel Module: vmalloc-backed Snapshot

### 7.1 Design

The `vmtainer_snap` kernel module creates `/dev/vmtainer_snap`, a character
device that allocates vmalloc memory and maps it to userspace. The snapshot
is loaded once into this always-resident buffer, eliminating page cache
misses on restore.

```c
// Allocate
ioctl(fd, VMTAINER_SNAP_ALLOC, &size);  // vmalloc_user(size)
// Map into userspace
void *buf = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
// remap_vmalloc_range() gives zero-copy access
```

### 7.2 Benchmark Results

Sparse copy of 29MB (7458 dirty pages) from various source types to a memfd:

```
Method                    Min      Avg      Max      Notes
------                    ---      ---      ---      -----
kmod vmalloc (warm)      5.77ms   6.22ms   9.12ms   Always-resident kernel pages
file mmap (warm)         5.74ms   6.80ms   8.19ms   Page cache hit
file mmap (cold)        50.28ms  94.39ms  109.2ms   Disk I/O (NVMe SSD)
file read (warm)         5.74ms   5.89ms   6.05ms   Buffered I/O, cache hit
file read (cold)         6.03ms   6.20ms   6.65ms   read() triggers readahead
```

**Key Finding**: When the snapshot is in the page cache (warm), all methods
converge to ~6ms -- this is the raw memcpy bandwidth for 29MB. The kmod
provides a **guaranteed warm-cache behavior** (no cold-cache penalty of
50-110ms), but the same effect can be achieved with `mlock()` or
`MAP_LOCKED` on a regular file mmap.

### 7.3 Conclusion

The vmalloc kernel module eliminates cold-cache variance but does not improve
warm-cache performance. For production use, pre-warming the snapshot via
`posix_fadvise(POSIX_FADV_WILLNEED)` or `mlock()` is simpler and equally
effective. The kmod is useful for **guaranteed latency SLOs** where page cache
eviction is a concern.

## 8. Parallel Clone Benchmark

### 8.1 Setup

- **Hardware**: Intel i5-14500 (14 cores / 20 threads), 40GB DDR5, NVMe SSD
- **Test**: Launch N VMs simultaneously from golden snapshot
- **Two modes**: "Restore-only" measures VMM restore time; "Full E2E"
  waits for the guest to finish execution (virtiofs mount + entrypoint + halt)

### 8.2 Restore-Only Results (VMM restore time only)

Each VM runs `vmtainer restore` with virtiofsd, measures the "restored in"
time. VMs continue executing until timeout (not part of measurement).

```
N      Success    Wall     Restore p50   virtiofsd p50   Snap p50   Throughput
----   -------    -----    -----------   -------------   --------   ----------
1      1/1        1.6s     13ms          1.5ms           12ms       0.6 VM/s
10     10/10      1.7s     17ms          1.4ms           15ms       5.9 VM/s
100    100/100    1.9s     57ms          6.8ms           40ms       52.5 VM/s
150    150/150    2.0s     86ms          20.5ms          47ms       73.3 VM/s
200    200/200    2.2s     48ms          10.1ms          24ms       92.3 VM/s
300    300/300    2.6s     132ms         38.5ms          46ms       114.0 VM/s
500    500/500    3.3s     71ms          22.0ms          28ms       153.4 VM/s
1000   1000/1000  5.0s     120ms         42.6ms          31ms       199.8 VM/s
```

### 8.3 Full E2E Results (guest runs /bin/true and halts)

Each VM has its own TAP device, IP address, virtiofsd, and runs `/bin/true`
inside an Alpine Linux chroot via virtiofs.

```
N      Success    Wall     Restore p50   Guest p50   Guest p99    Throughput
----   -------    -----    -----------   ---------   ---------    ----------
100    100/100    2.1s     73ms          1955ms      2122ms       46.8 VM/s
200    200/200    15.1s    164ms         2231ms      15054ms      13.3 VM/s
```

### 8.4 Detailed Breakdown at 100 Concurrent VMs (Full E2E)

```
Component              Min      Avg      p50      p90      p99      Max
---------              ---      ---      ---      ---      ---      ---
Total restore (ms)     25.7     83.7     73.4     163.0    298.2    387.3
  Snapshot copy        15.8     59.4     41.6     137.0    229.4    300.2
  KVM init              0.3      5.5      1.9      13.0     54.5     51.4
  virtiofsd startup     1.6     18.3     12.1      45.4     81.8    152.9
Wall time (ms)        1167    1846     1955      2104     2122     2257
```

### 8.5 Detailed Breakdown at 1000 Concurrent VMs (Restore-Only)

```
Component              Min      Avg      p50      p90      p99      Max
---------              ---      ---      ---      ---      ---      ---
Total restore (ms)     17.4    318.5    120.3     874.2   1325.2   1566.6
  Snapshot copy        12.4     57.1     31.3     121.2    417.5    663.8
  KVM init              0.3      5.7      2.8      13.0     41.0    105.3
  virtiofsd startup     1.3    255.7     42.6     703.9   1241.1   1469.4
```

### 8.6 Scaling Analysis

**virtiofsd is NOT the process-count bottleneck.** Even at 1000 VMs
(2000 total processes: 1000 vmtainer + 1000 virtiofsd), all VMs restore
successfully. The virtiofsd p50 stays reasonable (42ms at 1000 VMs) but
the p99 tail grows to 1.2s due to CPU scheduling contention.

**The real bottleneck at high concurrency is guest execution:**
- At 200+ VMs in full E2E mode, 200 vCPUs compete for 28 physical CPUs
- Each guest must: probe virtio devices, mount virtiofs (FUSE_INIT handshake),
  configure network (ip addr, ip route), chroot, run /bin/true, halt
- This guest work dominates wall clock time at high concurrency

**Memory bandwidth** for snapshot copy (29MB per VM) saturates around
200+ VMs. At 1000 VMs = 29GB total memcpy, the DDR5 bandwidth (~50GB/s)
becomes the limiting factor.

### 8.7 Recommendations for Production

1. **Batch launching**: Start VMs in waves of 50-100 to avoid overwhelming
   the scheduler
2. **CPU pinning**: Use `taskset` to pin each vmtainer+virtiofsd pair to
   a specific CPU set
3. **Pre-fork COW**: Load the snapshot once in a parent process and fork
   for each clone -- eliminates the 29MB per-VM memcpy entirely
4. **Reduce guest work**: Use a minimal init that skips network config
   when not needed

## 9. Guest Kernel Configuration

Minimal Linux 6.10 kernel with only required subsystems:

```
CONFIG_64BIT=y, CONFIG_X86_64=y
CONFIG_SMP=n                    # Single vCPU
CONFIG_PREEMPT_NONE=y           # No preemption overhead
CONFIG_SERIAL_8250=y            # Console
CONFIG_BLK_DEV_INITRD=y         # initrd support
CONFIG_VIRTIO=y, CONFIG_VIRTIO_MMIO=y           # virtio transport
CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y            # kernel cmdline probing
CONFIG_FUSE_FS=y, CONFIG_VIRTIO_FS=y            # virtiofs
CONFIG_VIRTIO_NET=y             # networking
CONFIG_NET=y, CONFIG_INET=y     # TCP/IP
CONFIG_UNIX=y                   # Unix domain sockets (nginx, etc.)
CONFIG_MODULES=n                # Static-only (faster boot)
CONFIG_HUGETLBFS=y              # Huge page support
```

Kernel command line:
```
console=ttyS0,115200n8 earlyprintk=serial,0x3f8,115200 nokaslr
virtio_mmio.device=0x1000@0xd0001000:5
virtio_mmio.device=0x1000@0xd0002000:6
```

## 10. CRI Plugin Architecture

The CRI (Container Runtime Interface) plugin enables Kubernetes to use
vmtainer as a container runtime.

```
kubelet --container-runtime-endpoint=unix:///var/run/vmtainer.sock
                    |
          +---------v----------+
          |  vmtainer-cri      |  Go gRPC server
          |                    |
          |  RuntimeService    |  RunPodSandbox, CreateContainer, etc.
          |  ImageService      |  PullImage, ListImages, etc.
          +---+-----+----+----+
              |     |    |
         +----+  +--+  +-+--------+
         |       |      |         |
    vmtainer  virtiofsd  TAP    OCI image
    process   process   device  cache
```

### 10.1 Pod Sandbox Lifecycle

```
RunPodSandbox    -> allocate TAP + IP, write config.json
CreateContainer  -> extract OCI image rootfs
StartContainer   -> vmtainer restore <snapshot> --config <config.json>
StopPodSandbox   -> SIGTERM vmtainer, cleanup TAP
RemovePodSandbox -> remove metadata + rootfs
```

### 10.2 Key Design Decisions

- **1:1 pod:VM mapping**: Each pod sandbox is one VM
- **Image cache**: OCI images extracted to `/var/lib/vmtainer/images/<hash>/rootfs/`
- **Metadata**: JSON files in `/var/lib/vmtainer/sandboxes/<id>/meta.json`
- **Networking**: TAP devices (`vmtap0..N`) with IP from configurable subnet
- **Exec**: Side-channel via virtiofs (write command JSON, poll for result)

## 11. Debug Logging

Debug output is available via the `--debug` flag. When enabled, the VMM
emits detailed trace information to stderr with `[DBG]` prefix:

```bash
vmtainer restore golden.snap --config config.json --debug
```

Example output:
```
[DBG] init: ram_mb=64 zero_ram=0
[DBG] init: ram=0x75d116600000 memfd=5 size=64MB
[DBG] vu_connect: connected after backoff=2000us
[DBG] vu_early_init: starting vhost-user handshake
[DBG] connect_tap: name=tap0 fd=9
[DBG] restore_snapshot: magic=0x48594c54534e4150 ver=7 ram_mb=64 bitmap=2048 bytes
[DBG] restore: bitmap copy 7458/16384 pages (29832KB)
[DBG] net_thread: started, tap_fd=9
[DBG] virtiofs STATUS: 0x0 -> 0x1
```

Logged events:
- KVM initialization (VM fd, vCPU fd, RAM allocation)
- virtiofsd connection timing (backoff steps)
- vhost-user handshake protocol
- TAP device setup
- Snapshot parse (header fields, bitmap stats)
- Sparse copy statistics (dirty page count)
- Virtio device status transitions
- Net thread lifecycle

## 12. Future Work

### 12.1 Demand-Paged Restore (userfaultfd) [IMPLEMENTED]

Implemented via Linux `userfaultfd(O_CLOEXEC | O_NONBLOCK)` with `UFFDIO_REGISTER_MODE_MISSING`.
Guest RAM memfd is registered with userfaultfd during `init()`. At restore time, only
the virtqueue rings (< 12 pages) are pre-faulted, completely skipping the 29.8MB upfront
memcpy. A background worker resolves guest memory access faults on demand (`UFFDIO_COPY`
for dirty snapshot pages, `UFFDIO_ZEROPAGE` for clean pages).
Achieved: **2.1ms - 3.4ms total VMM restore** (~0.3ms snapshot setup).

### 12.2 Pre-fork with COW

For the `clone` command, load the golden snapshot in a parent process and
`fork()` for each clone. The forked child inherits the parent's RAM via
COW (copy-on-write) page tables, avoiding the 29MB memcpy entirely.
The child then creates its own KVM VM and maps the inherited RAM.
Expected benefit: **~2ms restore** (KVM init + virtiofsd only, no memcpy).

### 12.3 Compressed Snapshots

Store only dirty pages in the snapshot file (skip zero pages entirely).
Would reduce file size from 65MB to ~30MB, improving cold-cache restore.
Could also apply LZ4 compression for further reduction.

### 12.4 Huge Pages (2MB)

Using 2MB huge pages for guest RAM would:
- Reduce TLB misses during memcpy (16 TLB entries vs 7458)
- Reduce page fault count during restore
- Improve guest execution performance

### 12.5 vhost-net Kernel Data Path

Currently virtio-net uses a userspace packet forwarding thread. Switching
to the kernel vhost-net data path (`/dev/vhost-net`) would:
- Eliminate user/kernel context switches per packet
- Reduce network latency
- Free one CPU thread per VM

## 13. File Inventory

```
src/vmm.cpp                    VMM implementation (C++17, ~2740 LOC)
src/boot.hpp                   Linux boot protocol helpers
src/bios_rom.h, bios.bin       Custom minimal BIOS
initrd_src/init                Guest init script (shell)
scripts/build_vmm.sh           Build script
scripts/build_kernel.sh         Kernel build script
scripts/build_initrd.sh         initrd build script
scripts/clone.sh               OCI image clone helper
images/bzImage                 Compiled kernel
images/initrd.cpio.gz          initrd with busybox + init
images/golden.snap             Golden snapshot (~65MB)
kmod/vmtainer_snap.c           Kernel module for resident snapshot
kmod/vmtainer_snap.h           Shared ioctl header
kmod/snap_bench.c              Restore benchmark tool
kmod/Makefile                  Kernel module makefile
cri/cmd/vmtainer-cri/main.go   CRI gRPC server
cri/pkg/runtime/runtime.go     RuntimeService implementation
cri/pkg/runtime/image.go       ImageService implementation
cri/pkg/vmm/vmm.go             vmtainer binary wrapper
cri/pkg/store/store.go          Metadata store
cri/pkg/network/cni.go          Network management
```
