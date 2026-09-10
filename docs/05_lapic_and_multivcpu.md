<!--
Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.

PROPRIETARY AND CONFIDENTIAL.
Unauthorized copying, reproduction, distribution, or modification of this
file, via any medium, is strictly prohibited.
All rights reserved.
-->

# 05 — Local APIC and Multi-vCPU

## 1. Why Multi-vCPU?

Modern operating systems and many applications expect multiple CPUs. A multi-vCPU VMM must create several vCPUs, start a thread for each, and coordinate their startup. The local APIC (LAPIC) is the key mechanism on x86 for interrupts and inter-processor interrupts (IPIs). KVM provides the in-kernel LAPIC as part of `KVM_CREATE_IRQCHIP`, and also allows `KVM_SET_LAPIC` to set or inspect the local APIC state.

## 2. The Local APIC

### What the LAPIC does

- Receives and prioritizes external interrupts from the IO-APIC.
- Sends and receives **inter-processor interrupts (IPIs)**.
- Maintains a local timer (APIC timer).
- Tracks the task-priority register (TPR), end-of-interrupt (EOI), and in-service (ISR) / interrupt-request (IRR) registers.

### APIC base register

The **local APIC base** is controlled by `MSR_IA32_APICBASE` (0x1b). On x86 it is usually `0xfee00000`. The base can be set in `struct kvm_sregs`:

```c
sregs.apic_base = 0xfee00000 | 0x800;  /* enabled, xAPIC, default base */
ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);
```

### `KVM_SET_LAPIC` and `KVM_GET_LAPIC`

These ioctls use `struct kvm_lapic_state`, which is a 1024-byte blob mirroring the local APIC page. Offsets 0x000..0x3ff are the APIC registers, each 16 bytes apart.

```c
struct kvm_lapic_state lapic;
if (ioctl(vcpu_fd, KVM_GET_LAPIC, &lapic) < 0)
    err(1, "KVM_GET_LAPIC");

/* APIC ID at offset 0x20 (bytes 0x20..0x23, but you write the whole 0x20 block). */
*(uint32_t *)(lapic.regs + 0x20) = apic_id << 24;  /* APIC ID in bits 24..31 */

/* Task-priority register at offset 0x80. */
*(uint32_t *)(lapic.regs + 0x80) = 0;

/* Spurious-interrupt vector at offset 0xf0. */
*(uint32_t *)(lapic.regs + 0xf0) = 0x1ff;

if (ioctl(vcpu_fd, KVM_SET_LAPIC, &lapic) < 0)
    err(1, "KVM_SET_LAPIC");
```

### Important APIC register offsets

| Offset | Register | Notes |
|--------|----------|-------|
| 0x00   | reserved | — |
| 0x20   | APIC ID  | Written by firmware; ID in bits 24..31 |
| 0x30   | APIC version | `0x60010` or `0x70010` |
| 0x80   | TPR      | Task-priority |
| 0x90   | APR      | Arbitration priority |
| 0xa0   | PPR      | Processor priority |
| 0xb0   | EOI      | Write to acknowledge an interrupt |
| 0xc0   | RRD      | Remote read |
| 0xd0   | LDR      | Logical destination |
| 0xe0   | DFR      | Destination format |
| 0xf0   | SVR      | Spurious interrupt vector; bit 8 = APIC software enable |
| 0x100  | ISR 0..7 | In-service |
| 0x180  | TMR 0..7 | Trigger mode |
| 0x200  | IRR 0..7 | Interrupt request |
| 0x280  | ESR      | Error status |
| 0x300  | ICR low  | Bits 0..31 of ICR |
| 0x310  | ICR high | Bits 32..63 of ICR |
| 0x320  | LVT timer | Local vector table: timer |
| 0x330  | LVT thermal | — |
| 0x340  | LVT PMC | — |
| 0x350  | LVT LINT0 | — |
| 0x360  | LVT LINT1 | — |
| 0x370  | LVT error | — |
| 0x380  | timer initial count | — |
| 0x3e0  | timer divide | — |

## 3. The ICR and IPIs

The **interrupt command register (ICR)** at offsets `0x300` and `0x310` is used to send IPIs. A vCPU writes to the ICR, and the target APIC(s) receive an interrupt. In a virtualized environment, KVM delivers IPIs between in-kernel local APICs automatically once the LAPIC is enabled.

A typical **INIT-SIPI-SIPI** sequence uses the ICR:

```c
#define APIC_BASE      0xfee00000
#define APIC_ID_OFFSET 0x20
#define APIC_ICR_LOW   0x300
#define APIC_ICR_HIGH  0x310

/* In guest physical memory at APIC_BASE. */

static void apic_write_icr(void *lapic_page, uint32_t hi, uint32_t lo)
{
    *(volatile uint32_t *)((char *)lapic_page + APIC_ICR_HIGH) = hi;
    *(volatile uint32_t *)((char *)lapic_page + APIC_ICR_LOW)  = lo;
}
```

- `lo` bits 0..7: vector
- `lo` bits 8..10: delivery mode (`000` fixed, `101` INIT, `110` SIPI)
- `lo` bits 11..14: destination mode (`0` physical, `1` logical)
- `lo` bits 18..19: shorthand (`01` self, `10` all incl. self, `11` all excl. self)
- `lo` bit 14: level (for INIT: must be `1`)
- `lo` bit 15: trigger mode
- `hi` bits 24..31: destination APIC ID

### Example: send an INIT IPI

```c
uint32_t dest = 0x01;        /* APIC ID of the target vCPU */
uint32_t lo = (5 << 0)       /* delivery mode INIT = 5 */
            | (1 << 14)      /* level = 1 */
            | (dest << 18);  /* shorthand all excl. self, or use ICR high */

apic_write_icr(lapic_page, dest << 24, lo);
```

### Example: send a SIPI

```c
/* SIPI = 6, vector = page number (e.g., 0x08 for 0x8000) */
uint32_t sipi_lo = (6 << 0) | (1 << 14) | (1 << 15) /* edge/level setup as needed */;
apic_write_icr(lapic_page, dest << 24, sipi_lo | (0x08));
```

KVM's in-kernel local APIC delivers these IPIs to the target vCPU's IRR. The target vCPU, if halted, will wake up and execute the SIPI handler.

## 4. Creating and Running Multiple vCPUs

### One vCPU per thread

The standard pattern is one pthread per vCPU. All vCPUs share the same guest memory and the same device state, but each has its own `vcpu_fd`, `kvm_run` mapping, and `struct kvm_regs`/`kvm_sregs`.

```c
struct vcpu {
    int            id;
    int            vcpu_fd;
    struct kvm_run *run;
    pthread_t      thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             running;
    int             halted;
};
```

### Creating the vCPUs

```c
for (int i = 0; i < num_vcpus; i++) {
    vcpus[i].id = i;
    vcpus[i].vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, i);
    if (vcpus[i].vcpu_fd < 0)
        err(1, "KVM_CREATE_VCPU %d", i);

    int msz = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    vcpus[i].run = mmap(NULL, msz, PROT_READ | PROT_WRITE,
                        MAP_SHARED, vcpus[i].vcpu_fd, 0);
    if (vcpus[i].run == MAP_FAILED)
        err(1, "mmap vCPU %d", i);
}
```

### The BSP and APs

For x86 SMP, the **bootstrap processor (BSP)** starts immediately from the boot vector. The **application processors (APs)** are started by `INIT-SIPI-SIPI` from the BSP. Your VMM must therefore:

1. Configure AP vCPUs with `KVM_SET_MP_STATE` set to `KVM_MP_STATE_UNINITIALIZED` or `KVM_MP_STATE_INITIAL`.
2. Mark the BSP as `KVM_MP_STATE_RUNNABLE`.
3. On APs, start the `KVM_RUN` loop. They will return `KVM_EXIT_HLT` immediately because they are not yet running.
4. The BSP sends `INIT` and `SIPI` IPIs.
5. KVM wakes the APs; they begin execution at the address given in the SIPI vector (vector * 0x1000).

```c
struct kvm_mp_state mp = { .mp_state = KVM_MP_STATE_RUNNABLE };
ioctl(vcpu[0].vcpu_fd, KVM_SET_MP_STATE, &mp);   /* BSP */

for (int i = 1; i < num_vcpus; i++) {
    mp.mp_state = KVM_MP_STATE_UNINITIALIZED;
    ioctl(vcpu[i].vcpu_fd, KVM_SET_MP_STATE, &mp);
}
```

### The vCPU thread

```c
void *vcpu_thread(void *arg)
{
    struct vcpu *v = arg;

    for (;;) {
        int ret = ioctl(v->vcpu_fd, KVM_RUN, 0);
        if (ret < 0) {
            if (errno == EINTR) continue;
            err(1, "KVM_RUN");
        }

        switch (v->run->exit_reason) {
        case KVM_EXIT_HLT:
            v->halted = 1;
            pthread_mutex_lock(&v->lock);
            pthread_cond_wait(&v->cond, &v->lock);
            v->halted = 0;
            pthread_mutex_unlock(&v->lock);
            break;

        case KVM_EXIT_IO:
            handle_io(v);
            break;

        case KVM_EXIT_MMIO:
            handle_mmio(v);
            break;

        case KVM_EXIT_SHUTDOWN:
            return NULL;

        case KVM_EXIT_INTERNAL_ERROR:
            errx(1, "vCPU %d internal error", v->id);

        default:
            errx(1, "vCPU %d unknown exit %llu",
                 v->id, (unsigned long long)v->run->exit_reason);
        }
    }
}
```

### Waking a halted vCPU

When an IPI is delivered, the in-kernel APIC sets the target vCPU's IRR. If the vCPU is blocked in `KVM_RUN` on a `HLT`, `KVM_RUN` returns `KVM_EXIT_IRQ_WINDOW_OPEN` or the pending interrupt is delivered and the vCPU resumes. In some setups, the VMM wakes the vCPU by toggling a `pthread_cond` or by calling `KVM_SET_MP_STATE` to `KVM_MP_STATE_RUNNABLE`.

Alternatively, you can call `KVM_NMI` or `KVM_INTERRUPT` before resuming `KVM_RUN`.

## 5. Per-CPU State

Every vCPU needs its own copies of:

- `struct kvm_regs` and `struct kvm_sregs`.
- `struct kvm_lapic_state` (if you override the in-kernel default).
- `struct kvm_run`.
- `struct kvm_debugregs` (if used).
- `struct kvm_xsave` (if used).

Shared state (under locks or atomics) includes:

- Guest memory.
- Device state and interrupt-pending bits.
- MMIO/PIO dispatch tables.
- `vm_running` and `vm_should_stop`.

A common pattern is to keep a global `vm` object and pass a pointer to it plus the vCPU index to each thread.

## 6. TPR, PPR, and cr8

### Task-priority register

`TPR` (offset `0x80` in the APIC page) controls which interrupts the vCPU accepts. The in-kernel LAPIC uses it automatically.

### `cr8`

In x86, `CR8` is a 64-bit register (only low 4 bits used) that shadows the TPR. Long-mode guests can `mov %cr8` to raise or lower task priority. KVM returns `cr8` in `run->cr8` after `KVM_RUN` and uses it to gate interrupt injection.

If you see `KVM_EXIT_TPR_ACCESS`, the guest tried to access the TPR; this is rare when the in-kernel APIC is enabled.

## 7. Spurious interrupts and EOI

After the guest finishes an ISR, it writes to the EOI register (offset `0xb0`). The in-kernel LAPIC handles EOI. If you are emulating a device that requires level-triggered EOI handling (e.g., `IO-APIC` redirection with `level` set), you can receive `KVM_EXIT_IOAPIC_EOI` and update the device.

```c
case KVM_EXIT_IOAPIC_EOI:
    device_eoi(run->eoi.vector);
    break;
```

The spurious-interrupt vector (SVR) at `0xf0` must have bit 8 set to enable the local APIC. Typical guest firmware sets it to `0x1ff`.

## 8. x2APIC

Modern guests may use **x2APIC** (MSR-based APIC) instead of the memory-mapped xAPIC. In x2APIC mode, the APIC is enabled and extended using `MSR_IA32_APICBASE` (bit 10). The guest reads and writes APIC registers via `MSR_IA32_X2APIC_*` (0x800..0x8ff). KVM's in-kernel x2APIC is enabled with the `KVM_CAP_X2APIC` capability.

For a minimal VMM, the legacy xAPIC is sufficient. If you expose a lot of vCPUs or run a recent Linux guest, you may also enable x2APIC.

## 9. Summary

- Each vCPU gets its own `KVM_CREATE_VCPU` and `kvm_run` mapping.
- The in-kernel LAPIC is part of `KVM_CREATE_IRQCHIP` and uses the APIC page at `0xfee00000`.
- Use `KVM_SET_LAPIC`/`KVM_GET_LAPIC` to inspect or modify the APIC page.
- IPIs use the ICR. The `INIT-SIPI-SIPI` sequence starts APs.
- Multi-vCPU requires one thread per vCPU, a BSP/AP distinction, and `KVM_SET_MP_STATE`.
- `KVM_EXIT_HLT` handling is the key to supporting multiple idling vCPUs.
- Shared guest memory and devices need synchronization between vCPU threads.
