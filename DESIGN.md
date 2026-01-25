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

## 3. Snapshot Format

### 3.1 Header (v7)

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

### 3.2 Dirty Page Bitmap

On snapshot save, every 4KB page of guest RAM is scanned with a fast
64-bit OR-reduce. Non-zero pages are marked in a bitmap (1 bit per page).
For 64MB RAM, the bitmap is 2048 bytes (16384 pages).

Typical dirty ratio: **~46%** (7458/16384 pages = 29MB of 64MB).

On restore, only dirty pages are copied, saving both time (skip zero-page
reads) and physical memory (memfd pages stay zero until written).

## 4. Snapshot Restore Pipeline

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

## 5. Optimization History

### 5.1 Timeline

| # | Optimization | Before | After | Speedup |
|---|-------------|--------|-------|---------|
| 1 | Baseline (full boot) | 800ms | 150ms | 5.3x (snapshot) |
| 2 | virtiofsd fast backoff (1ms exp) | 100ms wait | 1.5ms | 67x |
| 3 | Skip RAM zeroing on restore | 20ms KVM init | 0.5ms | 40x |
| 4 | mmap snapshot file (no read()) | 27ms | 25ms | 1.1x |
| 5 | Sparse copy (skip zero pages) | 25ms snap | 15ms | 1.7x |
| 6 | Dirty page bitmap in snapshot | 15ms snap | 12ms | 1.25x |
| 7 | Drop MAP_POPULATE (lazy faults) | 12ms snap | 12ms | ~same warm |

### 5.2 Current Breakdown (Single VM, Warm Cache)

```
Component           Time (ms)    % of total
---------           ---------    ----------
KVM init            0.3-0.8      5%
virtiofsd startup   1.2-1.6      12%
TAP connect         0.1          1%
RAM sparse copy     10-14        82%
  (29MB dirty pages)
KVM state restore   < 0.5        <4%
---------           ---------    ----------
Total               12-17        100%
```

### 5.3 Remaining Bottleneck

The dominant cost is **memcpy of ~29MB of dirty pages** from the snapshot
mmap into the memfd-backed guest RAM. This is ~6ms of pure memory bandwidth
(verified by the kernel module benchmark below), plus page fault overhead
when the source pages aren't in the page cache.

## 6. Kernel Module: vmalloc-backed Snapshot

### 6.1 Design

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

### 6.2 Benchmark Results

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

### 6.3 Conclusion

The vmalloc kernel module eliminates cold-cache variance but does not improve
warm-cache performance. For production use, pre-warming the snapshot via
`posix_fadvise(POSIX_FADV_WILLNEED)` or `mlock()` is simpler and equally
effective. The kmod is useful for **guaranteed latency SLOs** where page cache
eviction is a concern.

## 7. Parallel Clone Benchmark

### 7.1 Setup

- **Hardware**: Intel i5-14500 (14 cores / 20 threads), 40GB DDR5, NVMe SSD
- **Test**: Launch N VMs simultaneously from golden snapshot, each running
  `/bin/true` via virtiofs (Alpine Linux rootfs)
- **Metric**: VMM-reported restore time (from `vmtainer restore` output)

### 7.2 Results

```
Concurrency    Success   Wall Clock   Restore (p50)   Restore (p99)   Throughput
-----------    -------   ----------   -------------   -------------   ----------
1              1/1       1.6s         13ms            13ms            0.6 VM/s
10             10/10     1.7s         17ms            24ms            5.9 VM/s
50             50/50     1.9s         65ms            181ms           26.8 VM/s
100            100/100   2.1s         73ms            298ms           46.8 VM/s
```

### 7.3 Detailed Breakdown at 100 Concurrent VMs

```
Component              Min      Avg      p50      p90      p99      Max
---------              ---      ---      ---      ---      ---      ---
Total restore (ms)     25.7     83.7     73.4     163.0    298.2    387.3
  Snapshot copy        15.8     59.4     41.6     137.0    229.4    300.2
  KVM init              0.3      5.5      1.9      13.0     54.5     51.4
  virtiofsd startup     1.6     18.3     12.1      45.4     81.8    152.9
Wall time (ms)        1167    1846     1955      2104     2122     2257
```

### 7.4 Scaling Analysis

At 100 VMs on 28 logical CPUs:
- **Restore time scales linearly** with contention (~6x slower at 100 VMs
  vs 1 VM) due to memory bandwidth saturation (100 * 29MB = 2.9GB total
  memcpy) and CPU scheduling
- **virtiofsd contention** increases from 1.5ms (single VM) to 18ms avg
  (100 VMs) because 100 virtiofsd processes compete for CPU time
- **Throughput**: 46.8 VMs/sec aggregate, limited by total wall clock time
  of ~2.1 seconds (guest boot + virtiofs mount + entrypoint + halt)

### 7.5 Scaling Limits

150+ concurrent VMs showed instability (some VMs hung) due to:
- CPU oversubscription (150 vmtainer + 150 virtiofsd = 300 processes on 28 CPUs)
- virtiofsd connection timeout (backoff max 200ms may not be enough)
- Serial console output contention

**Recommendation**: For 100+ VMs, use batch launching (e.g., 50 at a time)
or a fork+COW model that shares the golden snapshot in the parent process.

## 8. Guest Kernel Configuration

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

## 9. CRI Plugin Architecture

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

### 9.1 Pod Sandbox Lifecycle

```
RunPodSandbox    -> allocate TAP + IP, write config.json
CreateContainer  -> extract OCI image rootfs
StartContainer   -> vmtainer restore <snapshot> --config <config.json>
StopPodSandbox   -> SIGTERM vmtainer, cleanup TAP
RemovePodSandbox -> remove metadata + rootfs
```

### 9.2 Key Design Decisions

- **1:1 pod:VM mapping**: Each pod sandbox is one VM
- **Image cache**: OCI images extracted to `/var/lib/vmtainer/images/<hash>/rootfs/`
- **Metadata**: JSON files in `/var/lib/vmtainer/sandboxes/<id>/meta.json`
- **Networking**: TAP devices (`vmtap0..N`) with IP from configurable subnet
- **Exec**: Side-channel via virtiofs (write command JSON, poll for result)

## 10. Debug Logging

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

## 11. Future Work

### 11.1 Demand-Paged Restore (userfaultfd)

Register the guest RAM memfd with userfaultfd and handle page faults lazily
from the snapshot. This would reduce restore time to near-zero (only the
pages actually touched by the guest during early boot would be copied).
Expected benefit: **0ms initial restore** + ~0.05ms per page fault.

### 11.2 Pre-fork with COW

For the `clone` command, load the golden snapshot in a parent process and
`fork()` for each clone. The forked child inherits the parent's RAM via
COW (copy-on-write) page tables, avoiding the 29MB memcpy entirely.
The child then creates its own KVM VM and maps the inherited RAM.
Expected benefit: **~2ms restore** (KVM init + virtiofsd only, no memcpy).

### 11.3 Compressed Snapshots

Store only dirty pages in the snapshot file (skip zero pages entirely).
Would reduce file size from 65MB to ~30MB, improving cold-cache restore.
Could also apply LZ4 compression for further reduction.

### 11.4 Huge Pages (2MB)

Using 2MB huge pages for guest RAM would:
- Reduce TLB misses during memcpy (16 TLB entries vs 7458)
- Reduce page fault count during restore
- Improve guest execution performance

### 11.5 vhost-net Kernel Data Path

Currently virtio-net uses a userspace packet forwarding thread. Switching
to the kernel vhost-net data path (`/dev/vhost-net`) would:
- Eliminate user/kernel context switches per packet
- Reduce network latency
- Free one CPU thread per VM

## 12. File Inventory

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
