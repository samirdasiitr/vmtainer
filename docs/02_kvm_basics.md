<!--
Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.

PROPRIETARY AND CONFIDENTIAL.
Unauthorized copying, reproduction, distribution, or modification of this
file, via any medium, is strictly prohibited.
All rights reserved.
-->

# 02 — KVM Basics: /dev/kvm, VM and vCPU Lifecycle, kvm_run

## 1. Opening the KVM Subsystem

The entry point to KVM in userspace is the character device `/dev/kvm`. A VMM opens it with read-write access, then uses `ioctl` to create and control a virtual machine.

```c
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

int kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
if (kvm_fd < 0) {
    perror("open /dev/kvm");
    return 1;
}
```

`/dev/kvm` is shared across all VM instances. You open it once per process, then use it to create many VMs. Each VM has its own `KVM_CREATE_VM` fd, and each vCPU its own `KVM_CREATE_VCPU` fd.

## 2. Checking KVM Capabilities

Before creating anything, the VMM should verify the API version and any required extension:

```c
int version = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
if (version != 12) {            /* current stable KVM API version */
    fprintf(stderr, "KVM API version mismatch: %d\n", version);
    return 1;
}

/* Check that KVM is present and supports an x86 host */
int kvmcap = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
if (kvmcap != 1) {
    fprintf(stderr, "KVM does not support user-defined memory regions\n");
    return 1;
}
```

Other useful capability checks:

- `KVM_CAP_IRQCHIP` — in-kernel PIC/IO-APIC.
- `KVM_CAP_IRQFD` — eventfd-based interrupt injection.
- `KVM_CAP_IOEVENTFD` — eventfd-based I/O notification.
- `KVM_CAP_EXT_CPUID` — extended CPUID handling.
- `KVM_CAP_NMI` — non-maskable interrupt support.

You use `KVM_CHECK_EXTENSION` with a capability number from `<linux/kvm.h>`.

## 3. Creating a Virtual Machine

A VM is created with the `KVM_CREATE_VM` ioctl:

```c
int vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
if (vm_fd < 0) {
    perror("KVM_CREATE_VM");
    return 1;
}
```

The resulting `vm_fd` is the file descriptor for the whole VM. All per-VM ioctls are issued on this fd, including memory region registration, vCPU creation, and interrupt controller setup.

### Setting the address-space identifier (ASI)

On x86 the machine type is usually set to the default. `KVM_CREATE_VM` can take a `type` argument, but for most hosted VMMs `0` is fine.

## 4. Allocating and Registering Guest Memory

A VM needs a guest-physical address space. With `KVM_SET_USER_MEMORY_REGION`, you tell the kernel which userspace `mmap` corresponds to a particular guest-physical (GP) range.

### Step 1: allocate a contiguous host memory buffer

```c
#include <sys/mman.h>

#define GUEST_RAM_SIZE (128ULL << 20)  /* 128 MB */

void *guest_mem = mmap(NULL, GUEST_RAM_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
if (guest_mem == MAP_FAILED) {
    perror("mmap guest RAM");
    return 1;
}
```

The buffer must be page-aligned and a multiple of the page size. On x86_64, allocate at least enough for the kernel, initrd, firmware, and devices. A 128 MiB or 1 GiB guest is common.

### Step 2: register the region with KVM

```c
struct kvm_userspace_memory_region region = {
    .slot = 0,
    .flags = 0,
    .guest_phys_addr = 0x0,
    .memory_size = GUEST_RAM_SIZE,
    .userspace_addr = (uint64_t)(uintptr_t)guest_mem,
};

int ret = ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
if (ret < 0) {
    perror("KVM_SET_USER_MEMORY_REGION");
    return 1;
}
```

### Slots, flags, and identity mapping

- `slot`: a small integer identifying the region. You can register multiple memory slots; the maximum is given by `KVM_CAP_NR_MEMSLOTS`.
- `guest_phys_addr`: the guest-physical base address.
- `memory_size`: the length of the region in bytes.
- `userspace_addr`: the virtual address in the VMM process where the backing memory begins.
- `flags`: `KVM_MEM_LOG_DIRTY_PAGES` to enable dirty logging. Other flags exist for hugepages or read-only regions (with `KVM_CAP_READONLY_MEM`).

A common convention is to place the first slot at `guest_phys_addr = 0` and make it cover all low guest memory. You can add high-memory slots later (for example, `0x100000000` for 64-bit RAM above 4 GiB).

### Accessing guest memory from the VMM

Because the VMM `mmap`s the guest RAM, you can read or modify it directly with normal pointer arithmetic. Just remember to convert guest-physical addresses to host-virtual addresses using the base of the mmap.

```c
uint8_t *gp_to_hv(uint64_t gpa, void *guest_mem)
{
    /* This example assumes the whole GPA space starts at 0 and maps to guest_mem. */
    return (uint8_t *)guest_mem + gpa;
}

void write_guest_u32(uint64_t gpa, uint32_t value, void *guest_mem)
{
    uint8_t *hva = gp_to_hv(gpa, guest_mem);
    *(uint32_t *)hva = value;
}
```

## 5. Creating a vCPU

Each vCPU is created with `KVM_CREATE_VCPU` on the VM fd:

```c
int vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);  /* vCPU id = 0 */
if (vcpu_fd < 0) {
    perror("KVM_CREATE_VCPU");
    return 1;
}
```

The second argument is the vCPU ID. On x86 it is typically the APIC ID / index. You must create vCPUs before the first `KVM_RUN` call.

### Mapping the kvm_run structure

After creating a vCPU, the VMM must map the shared `struct kvm_run` into userspace. This is the primary interface for exit information and I/O.

```c
#include <linux/kvm.h>

int mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
if (mmap_size < 0) {
    perror("KVM_GET_VCPU_MMAP_SIZE");
    return 1;
}

struct kvm_run *run = mmap(NULL, mmap_size,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, vcpu_fd, 0);
if (run == MAP_FAILED) {
    perror("mmap kvm_run");
    return 1;
}
```

`struct kvm_run` is a per-vCPU, kernel-userspace shared page. It contains:

- `request_interrupt_window`
- `immediate_exit`
- `exit_reason`
- `ready_for_interrupt_injection`
- `if_flag`
- `flags`
- `cr8`
- `apic_base`
- The union `s` (or anonymous union) that holds exit-specific details such as `io`, `mmio`, `system_event`, etc.

For 64-bit x86, the layout is in `<asm/kvm.h>` or `<linux/kvm.h>`. Always include the kernel headers rather than redefining the structure.

## 6. Initial vCPU State

Before running a vCPU, you typically set the initial registers. The most important are:

- General-purpose registers (`struct kvm_regs`): `rip`, `rsp`, `rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `rbp`, `r8..r15`, `rflags`.
- Segment and special registers (`struct kvm_sregs`): `cs`, `ds`, `es`, `fs`, `gs`, `ss`, `tr`, `ldt`, `gdt`, `idt`, `cr0`, `cr2`, `cr3`, `cr4`, `efer`, `apic_base`.
- FPU / XSAVE / debug registers (optional, usually with `KVM_SET_FPU`, `KVM_SET_XCRS`, `KVM_SET_DEBUGREGS`).

### Example: set real-mode boot state

If the guest will start in 16-bit real mode, as for an early BIOS or a bzImage boot sector:

```c
struct kvm_sregs sregs;
struct kvm_regs regs;

if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
    err(1, "KVM_GET_SREGS");

sregs.cs.selector = 0;
sregs.cs.base = 0;
sregs.cs.limit = 0xffff;
sregs.cs.g = 0;
sregs.cs.db = 0;    /* 16-bit code segment */

sregs.ds.selector = 0;
sregs.ds.base = 0;
sregs.ds.limit = 0xffff;

sregs.gdt.base = 0;
sregs.gdt.limit = 0xffff;
sregs.idt.base = 0;
sregs.idt.limit = 0xffff;

sregs.cr0 = 0x00000010;   /* PE=0, 16-bit real mode, no paging */
sregs.efer = 0;

if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
    err(1, "KVM_SET_SREGS");

memset(&regs, 0, sizeof(regs));
regs.rip = 0x7c00;        /* boot sector entry */
regs.rsp = 0x8f00;
regs.rflags = 0x2;        /* bit 1 reserved set */

if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0)
    err(1, "KVM_SET_REGS");
```

For a 64-bit long-mode Linux guest with a 64-bit protected-mode entry, set `cr0`/`cr4`/`efer` to enable paging and long mode, and point `rip` to the proper entry address.

## 7. The Basic vCPU Run Loop

The core of a VMM is the `KVM_RUN` loop. The kernel runs the vCPU in guest mode until an event (a VM exit) requires userspace attention. Then `KVM_RUN` returns with `run->exit_reason` populated.

```c
for (;;) {
    int ret = ioctl(vcpu_fd, KVM_RUN, 0);
    if (ret < 0) {
        if (errno == EINTR || errno == EAGAIN)
            continue;
        perror("KVM_RUN");
        return 1;
    }

    switch (run->exit_reason) {
    case KVM_EXIT_IO:
        handle_io(run, vcpu_fd);
        break;
    case KVM_EXIT_MMIO:
        handle_mmio(run);
        break;
    case KVM_EXIT_HLT:
        /* guest executed HLT; for now, stop */
        return 0;
    case KVM_EXIT_SHUTDOWN:
        fprintf(stderr, "Guest shutdown\n");
        return 0;
    case KVM_EXIT_FAIL_ENTRY:
        fprintf(stderr, "KVM fail entry: 0x%llx\n", run->fail_entry.hardware_entry_failure_reason);
        return 1;
    case KVM_EXIT_INTERNAL_ERROR:
        fprintf(stderr, "KVM internal error: %u\n", run->internal_error.suberror);
        return 1;
    default:
        fprintf(stderr, "Unknown exit reason: %u\n", run->exit_reason);
        return 1;
    }
}
```

When a vCPU is halted, you decide the policy. A single-vCPU VM often just exits. A multi-vCPU VM may put the thread to sleep until an interrupt or an `ioeventfd`/IPI wakes it.

## 8. Setting up the In-Kernel Local APIC / IRQCHIP

For most x86 VMs you want the in-kernel APIC and legacy interrupt controller. This enables `KVM_INTERRUPT`, `ioeventfd`, `irqfd`, and the local APIC clock.

```c
int ret = ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0);
if (ret < 0) {
    perror("KVM_CREATE_IRQCHIP");
    return 1;
}

struct kvm_pit_config pit = { .flags = KVM_PIT_SPEAKER_DUMMY };
ret = ioctl(vm_fd, KVM_CREATE_PIT2, &pit);
if (ret < 0) {
    perror("KVM_CREATE_PIT2");
    return 1;
}
```

- `KVM_CREATE_IRQCHIP` creates the in-kernel PIC, IO-APIC, and local APIC.
- `KVM_CREATE_PIT2` creates the 8254 PIT. The flag `KVM_PIT_SPEAKER_DUMMY` avoids exit on speaker ports.

After calling `KVM_CREATE_IRQCHIP`, `KVM_SET_LAPIC` and `KVM_SET_SREGS` with `apic_base` become especially important. You can still do APIC emulation in userspace by not calling `KVM_CREATE_IRQCHIP`, but for simplicity and performance this guide uses the in-kernel APIC.

## 9. A Minimal Complete Example

The following program creates a VM with 128 MiB of RAM, one vCPU, and an in-kernel IRQCHIP. It does not load any firmware; it only demonstrates the setup and a run loop that will quickly exit.

```c
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

int main(int argc, char **argv)
{
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) err(1, "/dev/kvm");

    int api = ioctl(kvm, KVM_GET_API_VERSION, 0);
    if (api != 12) errx(1, "KVM API version %d", api);

    int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) err(1, "KVM_CREATE_VM");

    const size_t ram_size = 128ULL << 20;
    void *ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) err(1, "mmap RAM");

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = ram_size,
        .userspace_addr = (uint64_t)(uintptr_t)ram,
    };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        err(1, "KVM_SET_USER_MEMORY_REGION");

    if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) < 0)
        err(1, "KVM_CREATE_IRQCHIP");

    struct kvm_pit_config pit = { .flags = KVM_PIT_SPEAKER_DUMMY };
    if (ioctl(vm, KVM_CREATE_PIT2, &pit) < 0)
        err(1, "KVM_CREATE_PIT2");

    int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) err(1, "KVM_CREATE_VCPU");

    int mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) err(1, "mmap kvm_run");

    /* Set minimal 16-bit real-mode state so we don't get a bad RIP. */
    struct kvm_sregs sregs;
    if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) err(1, "KVM_GET_SREGS");
    sregs.cs.selector = 0;
    sregs.cs.base = 0;
    sregs.ss.selector = 0;
    sregs.ss.base = 0;
    sregs.gdt.base = 0;
    sregs.gdt.limit = 0xffff;
    sregs.idt.base = 0;
    sregs.idt.limit = 0xffff;
    sregs.cr0 = 0x10;
    sregs.efer = 0;
    if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) err(1, "KVM_SET_SREGS");

    struct kvm_regs regs;
    if (ioctl(vcpu, KVM_GET_REGS, &regs) < 0) err(1, "KVM_GET_REGS");
    regs.rip = 0;
    regs.rsp = 0x9f000;
    regs.rflags = 0x2;
    if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) err(1, "KVM_SET_REGS");

    for (;;) {
        int ret = ioctl(vcpu, KVM_RUN, 0);
        if (ret < 0) {
            if (errno == EINTR) continue;
            err(1, "KVM_RUN");
        }

        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            printf("Halt\n");
            return 0;
        case KVM_EXIT_SHUTDOWN:
            printf("Shutdown\n");
            return 0;
        case KVM_EXIT_IO:
            printf("IO port 0x%x size %u dir %s\n",
                   run->io.port, run->io.size,
                   run->io.direction == KVM_EXIT_IO_OUT ? "out" : "in");
            break;
        case KVM_EXIT_FAIL_ENTRY:
            errx(1, "KVM fail entry: 0x%llx",
                 (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
        case KVM_EXIT_INTERNAL_ERROR:
            errx(1, "KVM internal error: %u", run->internal_error.suberror);
        default:
            errx(1, "exit reason %u", run->exit_reason);
        }
    }

    munmap(run, mmap_size);
    close(vcpu);
    close(vm);
    close(kvm);
    return 0;
}
```

Compile and run with:

```bash
cc -o kvm_basics kvm_basics.c
./kvm_basics
```

This program will likely print `Halt` immediately if the zero-filled RAM is interpreted as an `HLT` instruction, or `Fail entry` if the initial state is not accepted. It is a skeleton, not a bootable VM.

## 10. Summary of Key ioctls

| Ioctl | File descriptor | Purpose |
|-------|-----------------|---------|
| `KVM_GET_API_VERSION` | `/dev/kvm` | verify ABI |
| `KVM_CREATE_VM` | `/dev/kvm` | create a VM |
| `KVM_SET_USER_MEMORY_REGION` | `vm_fd` | map GPA to host memory |
| `KVM_CREATE_IRQCHIP` | `vm_fd` | create PIC/IO-APIC/LAPIC |
| `KVM_CREATE_PIT2` | `vm_fd` | create 8254 PIT |
| `KVM_CREATE_VCPU` | `vm_fd` | create a vCPU |
| `KVM_GET_VCPU_MMAP_SIZE` | `/dev/kvm` | size of `struct kvm_run` |
| `KVM_GET_SREGS` / `KVM_SET_SREGS` | `vcpu_fd` | segment/special registers |
| `KVM_GET_REGS` / `KVM_SET_REGS` | `vcpu_fd` | GP registers |
| `KVM_RUN` | `vcpu_fd` | run the vCPU |

These building blocks — a VM, memory, an IRQ chip, a vCPU, and a `kvm_run` loop — are the foundation for every later chapter.
