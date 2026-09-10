<!--
Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.

PROPRIETARY AND CONFIDENTIAL.
Unauthorized copying, reproduction, distribution, or modification of this
file, via any medium, is strictly prohibited.
All rights reserved.
-->

# 01 — Overview: Building a Virtual Machine Monitor with KVM

## 1. What is a VMM?

A **Virtual Machine Monitor (VMM)**, also called a **hypervisor**, is a program that creates, manages, and executes one or more **virtual machines (VMs)**. A VMM presents each guest with an abstracted view of hardware: virtual CPUs (vCPUs), virtual memory, virtual I/O devices, and virtual firmware. From the guest's perspective, it appears to be running on real hardware, even though every access to privileged state is mediated by the VMM and the underlying hardware virtualization extension.

There are two broad categories of VMMs:

- **Type 1 (bare-metal) hypervisors**: Run directly on physical hardware (e.g., VMware ESXi, Microsoft Hyper-V, Xen). They usually operate in the most privileged mode and own device drivers.
- **Type 2 (hosted) hypervisors**: Run as a normal process on top of a host operating system (e.g., QEMU, VirtualBox, HVF on macOS). They rely on the host kernel for scheduling, memory management, and device access.

This guide focuses on building a **hosted, hardware-assisted VMM using Linux KVM**. In this model, a userspace process creates a virtual machine by interacting with the kernel through the `/dev/kvm` device and the KVM API, and performs I/O and device emulation in userspace.

## 2. The Role of KVM

**KVM (Kernel-based Virtual Machine)** is a Linux kernel subsystem that turns the kernel into a Type 1-like hypervisor while userspace remains responsible for policy, device emulation, and lifecycle management. The architecture is usually described as a **split hypervisor**:

```
+-------------------------------------------------------------+
|  Guest OS (VM)                                              |
|  +-----------------+  +-----------------+                   |
|  |     vCPU 0      |  |     vCPU 1      |  ...              |
|  +-----------------+  +-----------------+                   |
+-------------------------------------------------------------+
|  KVM kernel module  (VMX/SVM, shadow page tables, VM exits)  |
+-------------------------------------------------------------+
|  /dev/kvm  (KVM API)                                        |
+-------------------------------------------------------------+
|  Userspace VMM  (QEMU, your custom VMM)                     |
|  - device emulation, memory layout, firmware, boot loading  |
+-------------------------------------------------------------+
|  Host Linux                                                 |
+-------------------------------------------------------------+
```

KVM is not itself a complete VMM. It is an **engine for running virtualized CPU instructions**. It exposes a set of `ioctl`s through `/dev/kvm` that a userspace program uses to:

1. Create a virtual machine (`KVM_CREATE_VM`).
2. Map guest physical memory (`KVM_SET_USER_MEMORY_REGION`).
3. Create and run virtual CPUs (`KVM_CREATE_VCPU`).
4. Set and get vCPU state (`KVM_SET_REGS`, `KVM_SET_SREGS`, `KVM_GET_REGS`, etc.).
5. Inject interrupts, configure the local APIC, and manage I/O (`KVM_INTERRUPT`, `KVM_SET_LAPIC`, `ioeventfd`, `irqfd`).

The userspace VMM must implement the rest: firmware, memory layout, I/O device models, boot protocol, disk and network devices, ACPI tables, and any paravirtualized devices.

## 3. What KVM Does vs. What Userspace Does

### KVM (kernel side)

- Uses CPU hardware virtualization extensions: **Intel VT-x (VMX)** or **AMD-V (SVM)**.
- Allocates and manages VMCS (Intel) or VMCB (AMD) control structures.
- Runs guest code in **non-root / guest mode**.
- Traps **sensitive instructions and events** (VM exits) back to the kernel and, if necessary, to userspace.
- Handles many exit types in the kernel: I/O, HLT, MMIO, MSR, CPUID, interrupts, APIC, NMI, SHUTDOWN, etc.
- Provides in-kernel acceleration for APIC, PIT, PIC, local APIC, and IO-APIC.
- Implements `kvm_run`, a shared memory structure for communication between kernel and userspace.

### Userspace VMM

- Opens `/dev/kvm` and configures the VM.
- Allocates guest-physical RAM as an anonymous `mmap` and registers it with KVM.
- Loads a guest firmware (BIOS/UEFI) or operating system image into memory.
- Creates vCPUs, sets initial register state, and provides a per-vCPU `kvm_run` mapping.
- Implements the main **vCPU run loop**: calls `KVM_RUN` and dispatches on `kvm_run->exit_reason`.
- Emulates devices (serial port, 8254 PIT, RTC, PIC, APIC, PCI, virtio, disk, network).
- Handles guest port I/O, MMIO, and MSRs by reading or writing the `kvm_run` structure.
- Injects interrupts and inter-processor interrupts (IPIs).
- Builds ACPI tables and a memory map (E820) for the guest.
- Loads Linux bzImage, initrd, and boot parameters for a paravirtualized or fully virtualized system.

## 4. Scope of This Guide

This guide is a hands-on, tutorial-style reference for writing a minimal but functional VMM with the Linux KVM API. It is written for systems programmers who are comfortable with C/C++, Linux system programming, and the x86/x86_64 architecture.

The guide assumes:

- A Linux host with `CONFIG_KVM`, `CONFIG_KVM_INTEL` or `CONFIG_KVM_AMD` enabled.
- x86_64 guests (32-bit protected mode is discussed for boot loading).
- Userspace is written in C or C++ and uses `ioctl` and `mmap`.
- The VMM is not tied to QEMU; it is built from `KVM_*` ioctls directly.

### Topics covered

1. **Overview** — what a VMM is, KVM's split design, and what you will build.
2. **KVM basics** — `/dev/kvm`, creating a VM, creating a vCPU, mapping memory, the `kvm_run` loop, and basic lifecycle.
3. **VM exits** — exit reasons, how to handle I/O, HLT, MMIO, MSR, CPUID, SHUTDOWN, and internal errors.
4. **I/O device simulation** — port I/O, MMIO, `ioeventfd`, `irqfd`, PIC/APIC basics, and an emulated serial/RTC.
5. **Local APIC and multi-vCPU** — LAPIC, IPI, INIT/SIPI, per-CPU state, running multiple vCPUs.
6. **VFIO** — PCI device passthrough, IOMMU groups, container, DMA mapping.
7. **ACPI and BIOS** — firmware, Bochs BIOS, shutdown port, building ACPI tables.
8. **Kernel loading** — bzImage structure, setup header, E820, protected/long mode entry.
9. **Disk images and `bximage`** — creating and attaching disk images.

### What this guide is not

- It is not a full QEMU replacement; it is a learning path to understand the KVM API from scratch.
- It does not cover kernel-side KVM internals in full depth.
- It does not discuss every possible ARM/aarch64 or other architecture; x86_64 is the target.
- It does not implement a complete virtio or block driver (only the concepts and minimal examples).

## 5. A Minimal VMM Architecture

At the highest level, a KVM-based VMM looks like this:

```
main()
  ├── open("/dev/kvm", O_RDWR)
  ├── KVM_GET_API_VERSION
  ├── kvm_create_vm()
  ├── allocate_guest_memory()
  ├── KVM_SET_USER_MEMORY_REGION
  ├── create_firmware_and_acpi_tables()
  ├── load_kernel_and_initrd()
  ├── for each vCPU:
  │     KVM_CREATE_VCPU
  │     mmap kvm_run
  │     set initial registers
  │     start pthread
  │     vcpu_run_loop()
  │         while running:
  │             KVM_RUN
  │             switch (kvm_run->exit_reason):
  │                 KVM_EXIT_IO:       emulate_port_io()
  │                 KVM_EXIT_MMIO:     emulate_mmio()
  │                 KVM_EXIT_HLT:      idle / halt vcpu
  │                 KVM_EXIT_SHUTDOWN: stop VM
  │                 ...
  └── wait for vCPUs and clean up
```

This structure is the foundation of every file that follows. Each file expands one part of the diagram: memory, exits, I/O, the APIC, VFIO, firmware, kernel loading, and storage.

## 6. Why Build from Scratch?

Writing a VMM from the KVM API up is the best way to understand:

- How hardware virtualization translates between guest and host state.
- How a guest physical address space is constructed.
- What a VM exit is and why efficient exit handling matters.
- How device models translate between legacy I/O and modern paravirtual I/O.
- How an operating system actually starts: boot protocols, E820, ACPI, APIC, and long mode.

By the end of the guide, you will have a reusable mental model and a working code skeleton for a small but real VMM.

## 7. Summary

- A VMM creates and manages virtual machines.
- KVM is a Linux kernel module that executes virtualized CPUs and handles hardware-assisted traps.
- Userspace owns device emulation, boot loading, memory layout, and lifecycle.
- This guide teaches x86_64 KVM userspace programming from first principles.
