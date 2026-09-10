# example-vmm — a self-contained KVM learning skeleton

This directory contains a tiny, heavily-commented x86_64 Virtual Machine Monitor
built directly on the Linux KVM API.  It is intentionally self-contained and does
**not** depend on the rest of the `vmtainer` source tree.  It only needs `gcc`,
`make`, and a Linux host with `/dev/kvm`.

## What it does

* Opens `/dev/kvm`, creates a VM, and creates at least two vCPUs.
* Allocates a single contiguous guest RAM region and maps it at guest physical 0.
* Loads an x86 `bzImage` Linux kernel and an optional `initrd` into the right
  places in guest memory (0x10000 for the real-mode setup, 0x100000 for the
  protected payload, and high RAM for the initrd).
* Fills in the `setup_header` and `e820` memory map fields the boot protocol
  expects.
* Constructs minimal but valid ACPI tables (RSDP, RSDT, XSDT, MADT, FADT, DSDT)
  and places them in the reserved low memory the kernel scans.
* Writes a tiny 16-bit real-mode reset vector at 0xFFFF0 that far-jumps to the
  bzImage start at 0x1000:0.
* Runs each vCPU in its own `pthread`.  vCPU0 starts at the reset vector; extra
  vCPUs are parked on a tiny `hlt` stub so the VM does not immediately trip over
  an un-emulated local APIC.
* Handles the most common VM exits: `KVM_EXIT_HLT`, `KVM_EXIT_SHUTDOWN`,
  `KVM_EXIT_IO` (serial/COM1 output is printed to stderr), and `KVM_EXIT_MMIO`.
* Supports a runtime debug/verbosity flag and demonstrates KVM debug-register
  `KVM_SET_DEBUG_REGS` / `KVM_GET_DEBUG_REGS` calls.

This is a **learning skeleton**, not a production VMM.  It will not boot a full
modern distribution, but the structure is correct for a small `bzImage + initrd`.

## Build

```bash
cd /home/sadas/workspace/vmtainer/docs/example-vmm
make
```

The build produces a single `vmm` binary.  No external build tools are used.

## How to run

You need `/dev/kvm` access (root or membership in the `kvm` group).  Provide a
small x86_64 `bzImage` and, optionally, a `cpio.gz` initrd.

```bash
# basic: 128 MiB, 2 vCPUs, built-in command line
sudo ./vmm -k /path/to/bzImage -i /path/to/initrd

# with custom guest memory, vCPU count, and kernel command line
sudo ./vmm -k /path/to/bzImage \
           -i /path/to/initrd \
           -m 256 \
           -s 2 \
           -c "console=ttyS0,115200n8 loglevel=7"

# verbose per-step logging plus debug register dump
sudo ./vmm -d -k /path/to/bzImage -i /path/to/initrd
```

Options:

```
-k <path>   bzImage kernel (required)
-i <path>   initrd (optional)
-m <MB>     guest RAM in megabytes (default 128)
-s <N>      number of vCPUs (default 2)
-c <cmd>    kernel command line (default console=ttyS0,115200n8)
-d          enable extra DEBUG() logging and show debug registers
-h          show help
```

## Expected output

A successful run looks roughly like this:

```
[example-vmm] opening /dev/kvm
[example-vmm] KVM API version: 12
[example-vmm] guest memory 134217728 bytes at 0x7f...
[example-vmm] installed BIOS reset vector and AP halt stub
[example-vmm] installing minimal ACPI tables
[example-vmm] loading kernel from /path/to/bzImage
[example-vmm] setup_sects=27 setup_size=14336
[example-vmm] payload off=14336 len=1234567
[example-vmm] initrd at 0x07c00000 size 1048576
[example-vmm] created vCPU 0 (BSP)
[example-vmm] created vCPU 1 (AP)
[example-vmm] vCPU 0 starting (BSP=1)
[example-vmm] vCPU 1 starting (BSP=0)
... any serial output from the kernel will appear here ...
[example-vmm] vCPU 0 exited: HLT
[example-vmm] vCPU 1 exited: HLT
[example-vmm] all vCPUs stopped, cleaning up
```

If the kernel is built with `console=ttyS0`, the characters written to COM1
port 0x3F8 are printed to the VMM's `stderr`.

## Limitations

* **No APIC / PIC / IRQ controller** is emulated.  The second (and any further)
  vCPU is parked on a `hlt` stub.  If you want to stress the multi-CPU path,
  pass `nosmp` or `maxcpus=1` on the kernel command line.
* **No real devices** are emulated.  Only a trivial serial-output trap on
  `KVM_EXIT_IO` exists.
* **The DSDT is a stub** — it contains only an ACPI table header, not a real
  DSDT bytecode.  This is enough to show table placement and linking.
* **It will not boot a full distro**; it is intended for small `bzImage + initrd`
  images compiled for a simple `console=ttyS0` console.
* **Only x86_64** is supported.
* **Real mode only at entry**; the VMM does not implement protected- or
  long-mode support beyond what the host already provides.

## Files

| File      | Purpose                                                     |
|-----------|-------------------------------------------------------------|
| README.md | This file                                                   |
| Makefile  | Builds the `vmm` binary                                     |
| vmm.h     | VMM constants, structures, and declarations                 |
| vmm.c     | `main`, CLI, VM/vCPU lifecycle, run loop, multi-vCPU threads|
| kernel.h  | bzImage / initrd / E820 declarations                        |
| kernel.c  | Parse setup header, load image, initrd, boot params, E820   |
| acpi.h    | ACPI table structures and `acpi_install()`                  |
| acpi.c    | Construct RSDP/RSDT/XSDT/MADT/FADT/DSDT                     |
| bios.h    | BIOS reset-vector declarations                              |
| bios.c    | In-memory reset vector and AP halt stub                     |
