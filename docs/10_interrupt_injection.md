# Interrupt Injection in a KVM VMM

Delivering interrupts to a virtual machine is one of the most performance-sensitive jobs of a VMM. KVM offers several injection paths, from the simple legacy 8259A Programmable Interrupt Controller (PIC), through the local APIC used by multi-processor guests, to MSI/MSI-X message-signalled interrupts and eventfd-based fast paths. This document explains each mechanism and gives code-level examples.

---

## 1. The high-level picture

A physical x86 machine has at least three layers of interrupt delivery:

1. **Sources** — devices raise an interrupt request line or write a message.
2. **I/O interrupt controller** — historically the 8259 PIC or the I/O APIC routes the request to a CPU.
3. **Local APIC** — each CPU has a Local APIC that receives the request, prioritizes it, and presents it to the vCPU core.

KVM can model all three in the kernel, or the VMM can drive some of it from userspace. A minimal single-vCPU VM often uses:

* `KVM_CREATE_IRQCHIP` to instantiate the in-kernel 8259/PIC.
* `KVM_INTERRUPT` (legacy real-mode) or the local-APIC page to deliver vectors.

A multi-vCPU or PCI-passthrough setup usually uses:

* A kernel I/O APIC (`KVM_CREATE_IRQCHIP` or explicit `KVM_SET_GSI_ROUTING`).
* Local APIC pages (`KVM_GET_LAPIC` / `KVM_SET_LAPIC`) for per-CPU delivery.
* `KVM_SIGNAL_MSI` or `KVM_IRQFD` for PCI MSI/MSI-X.

---

## 2. Creating the in-kernel interrupt controllers

The easiest way to get 8259, I/O APIC and local APICs is to ask KVM to create them.

```c
/* after KVM_CREATE_VM, before creating vCPUs */
int ret = ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0);
if (ret < 0) {
    perror("KVM_CREATE_IRQCHIP");
    return -1;
}
```

`KVM_CREATE_IRQCHIP` creates:

* two cascaded 8259 PICs
* one I/O APIC
* a virtual PIT if you have also called `KVM_CREATE_PIT2`

If you do not call `KVM_CREATE_IRQCHIP`, your VMM must either emulate the controllers in userspace or use the local-APIC only (`KVM_CAP_IRQCHIP` may also need to be checked, but it is always present on x86 today).

---

## 3. Legacy PIC interrupt injection: `KVM_INTERRUPT`

For a real-mode guest, or any guest that has not yet enabled the local APIC, the `KVM_INTERRUPT` ioctl is the simplest injection primitive.

```c
struct kvm_interrupt intr = {
    .irq = 32   /* the *vector* the guest will see in the IDT */
};

ioctl(vcpu_fd, KVM_INTERRUPT, &intr);
```

Important restrictions:

* `KVM_INTERRUPT` works only when the local APIC is in `xAPIC` legacy mode or the guest has not enabled it; once the guest switches to `x2APIC`, use APIC-page writes.
* The vector must be in the range 0–255.
* It does not route through the 8259; it is a CPU-local *event*.

For 8259 PIC routing, use `KVM_IRQ_LINE` instead.

---

## 4. GSI / line-based interrupts: `KVM_IRQ_LINE`

Global System Interrupt (GSI) numbers map physical IRQ sources to either the 8259 (0–15) or the I/O APIC (0+).

```c
struct kvm_irq_level {
    __u32  irq;     /* GSI number */
    __u32  level;   /* 0 = de-assert, 1 = assert */
};

/* assert an ISA IRQ, e.g. IRQ4 for a serial port */
struct kvm_irq_level serial = { .irq = 4, .level = 1 };
ioctl(vm_fd, KVM_IRQ_LINE, &serial);
```

Edge-triggered devices must de-assert and re-assert to deliver a second pulse:

```c
serial.level = 0;
ioctl(vm_fd, KVM_IRQ_LINE, &serial);
serial.level = 1;
ioctl(vm_fd, KVM_IRQ_LINE, &serial);
```

For I/O APIC level-triggered pins (PCI INTx, for example), the level can be left asserted. The I/O APIC will keep sending the interrupt until the guest clears the source.

`KVM_IRQ_LINE_STATUS` returns whether the interrupt was coalesced or accepted.

---

## 5. Local APIC injection

When a guest enables the local APIC (MSR `IA32_APIC_BASE`, bit 11), the CPU expects interrupts through the local APIC page. KVM exposes this page as a 4 KiB region at guest physical address 0xFEE00000 by default.

You can read and write the local APIC state with:

```c
struct kvm_lapic_state lapic;
ioctl(vcpu_fd, KVM_GET_LAPIC, &lapic);
/* modify lapic.regs[...] */
ioctl(vcpu_fd, KVM_SET_LAPIC, &lapic);
```

The `lapic.regs` array is a 1024-byte (0x400) view of the APIC page, in the same layout the x86 APIC uses:

| Offset | Register | Meaning |
|--------|----------|---------|
| 0x020  | APIC_ID  | local APIC id |
| 0x030  | APIC_VER | version |
| 0x080  | TPR      | task-priority |
| 0x0B0  | EOI      | end of interrupt, write 0 to clear top ISR |
| 0x0D0  | LDR      | logical destination |
| 0x0E0  | DFR      | destination format |
| 0x0F0  | SVR      | spurious vector, bit 8 = APIC enable |
| 0x200  | IRR0     | interrupt request register 0–255 |
| 0x280  | ISR0     | in-service register 0–255 |
| 0x300  | ICR0     | interrupt command register, bits 0–31 |
| 0x310  | ICR1     | interrupt command register, bits 32–63 |
| 0x320  | LVT_TIMER | local vector table timer |
| 0x350  | LVT_LINT0 | LVT LINT0 |
| 0x360  | LVT_LINT1 | LVT LINT1 |
| 0x370  | LVT_ERROR | LVT error |

### 5.1 IPI through the ICR

The `KVM_SET_LAPIC` write can be used to set up the local APIC. To send an IPI from one vCPU to another, the source vCPU performs a `send` operation by writing the ICR low word (`0x300`) with a vector and a destination shorthand. In hardware, this is a CPU write. In KVM it is usually the guest that writes, but the VMM can prime the APIC page through `KVM_SET_LAPIC`.

```c
/* prime an IPI: vector 0xEF to all-but-self, fixed delivery */
/* ICR layout: bits 0-7 vector, 8-10 delivery mode, 18-19 dest shorthand */
#define ICR_LOW 0x300
#define ICR_HI  0x310

uint8_t *regs = lapic.regs;
*(uint32_t *)(regs + ICR_HI) = 0x00000000;    /* destination: all but self */
*(uint32_t *)(regs + ICR_LOW) = 0x000C00EF;    /* fixed, all-but-self, vector 0xEF */

ioctl(vcpu_fd, KVM_SET_LAPIC, &lapic);
```

### 5.2 Direct vector injection through the APIC page

For a *level* or *fixed* interrupt from an I/O APIC to a specific CPU, you can set the matching bit in the target local APIC's `IRR` (offset 0x200) and then `KVM_SET_LAPIC`. KVM will evaluate the new bit on the next `KVM_RUN` and inject the vector if it can.

A simpler and more common path is `KVM_INTERRUPT` or the `KVM_NMI`/`KVM_SET_LAPIC` flows.

### 5.3 Spurious vector and TPR

The local APIC must be enabled or it will ignore external interrupts:

```c
uint32_t svr = *(uint32_t *)(regs + 0x0F0);
svr |= 0x100;   /* APIC software enable */
*(uint32_t *)(regs + 0x0F0) = svr;
```

Setting the `TPR` (Task-Priority Register) to a higher number masks lower-priority interrupts until the guest lowers it.

---

## 6. MSI / MSI-X and `KVM_SIGNAL_MSI`

Message Signalled Interrupts are memory writes to a special address. PCI devices using MSI/MSI-X do not assert a physical line; instead they write a 32-bit (or 64-bit) message containing the vector and an address derived from the `Address` and `Data` fields of the MSI capability.

KVM handles this with `KVM_SIGNAL_MSI`:

```c
struct kvm_msi msi = {
    .address_lo = 0xFEE00000,      /* standard LAPIC address */
    .address_hi = 0x00000000,
    .data       = 0x000000EF,      /* vector 0xEF, edge triggered, fixed */
    .flags      = 0,               /* or KVM_MSI_VALID_DEVID etc. */
};

ioctl(vm_fd, KVM_SIGNAL_MSI, &msi);
```

For 64-bit MSI, fill both `address_lo` and `address_hi`. KVM resolves the destination local APIC from the address and delivers the vector. This is the path used for `irqfd`-based devices and for VFIO PCI passthrough.

---

## 7. Fast paths: `ioeventfd` and `irqfd`

A userspace device can inject an interrupt from another thread without a `KVM_RUN` exit by using `irqfd`.

### 7.1 `irqfd`

`KVM_IRQFD` registers an `eventfd` that, when signalled, injects a GSI into the guest.

```c
int ev = eventfd(0, EFD_CLOEXEC);

struct kvm_irqfd irqfd = {
    .fd      = ev,
    .gsi     = 4,       /* the GSI to inject */
    .flags   = 0,
};

ioctl(vm_fd, KVM_IRQFD, &irqfd);

/* later, in another thread / process: */
uint64_t one = 1;
write(ev, &one, sizeof(one));   /* injects GSI 4 */
```

For MSI, use `KVM_IRQFD` with `flags = KVM_IRQFD_FLAG_MSI` and set `msi` fields.

### 7.2 `ioeventfd`

`ioeventfd` is the opposite: it lets a guest write to a memory-mapped or port-IO region without exiting to userspace. A guest write to a registered address triggers an `eventfd` that the VMM or a backend (e.g. `vhost`) can consume.

```c
int ev = eventfd(0, EFD_CLOEXEC);

struct kvm_ioeventfd iofd = {
    .fd      = ev,
    .addr    = 0xE0000000,       /* guest physical address */
    .len     = 4,
    .flags   = 0,
    .datamatch = 0,
};

ioctl(vm_fd, KVM_IOEVENTFD, &iofd);
```

---

## 8. NMI, exception injection, and INIT/SIPI

### 8.1 Non-maskable interrupt

```c
ioctl(vcpu_fd, KVM_NMI, 0);
```

`KVM_NMI` is the equivalent of the `LINT1`-NMI pin. The guest vector is taken from the `LVT_LINT1` register unless the APIC is disabled, in which case the standard NMI vector 2 is used.

### 8.2 Exceptions and software interrupts

Use `KVM_SET_VCPU_EVENTS` (or older `KVM_INTERRUPT`) to request an exception, e.g. `#GP` (general protection fault) or `INT3` (#BP). For exception injection:

```c
struct kvm_vcpu_events events;
memset(&events, 0, sizeof(events));

/* request a #GP(0) */
events.exception.injected = 1;
events.exception.nr       = 13;   /* #GP */
events.exception.has_error_code = 1;
events.exception.error_code     = 0;
events.exception.pad            = 0;

ioctl(vcpu_fd, KVM_SET_VCPU_EVENTS, &events);
```

### 8.3 INIT/SIPI for APs

When bringing up an Application Processor (AP), the BSP sends an `INIT` IPI followed by two `SIPI` IPIs. KVM models this through the local APIC `ICR` writes and the `KVM_SET_MP_STATE` ioctl.

```c
/* Put the AP into wait-for-SIPI state */
struct kvm_mp_state mp = { .mp_state = KVM_MP_STATE_WAIT_SIPI };
ioctl(vcpu_fd, KVM_SET_MP_STATE, &mp);

/* Guest BSP writes SIPI ICR. KVM then places AP in MP_STATE_RUNNABLE. */
```

On first `KVM_RUN` the AP vCPU starts at the SIPI vector (real-mode, `CS:IP` based on the 8-bit vector shifted left 12).

---

## 9. Interrupt-window open and pending interrupts

A vector cannot be delivered if the guest has `IF=0` (interrupts disabled) in `RFLAGS`, or if a higher-priority task-priority is set. When the guest re-enables interrupts, KVM may exit with `KVM_EXIT_IRQ_WINDOW_OPEN`.

```c
/* in your vCPU loop */
switch (run->exit_reason) {
case KVM_EXIT_IRQ_WINDOW_OPEN:
    /* re-try injecting any pending interrupt now that IF became 1 */
    inject_pending_interrupts(vcpu);
    break;
}
```

Most of the time KVM handles this invisibly: if an interrupt is pending and `KVM_RUN` is re-entered, it will be delivered as soon as the vCPU can take it. You only need `KVM_EXIT_IRQ_WINDOW_OPEN` if you are doing manual injection (e.g. with `KVM_INTERRUPT`).

---

## 10. End-of-interrupt (EOI) and in-kernel handling

In a fully in-kernel APIC setup, the guest writes the APIC `EOI` register. KVM then lowers the interrupt in the in-service register (ISR) and, for level-triggered I/O APIC pins, may de-assert the GSI. If you are emulating the I/O APIC or PIC in userspace, you must hook `KVM_EXIT_IO` or `KVM_EXIT_MMIO` for the EOI port and clear the in-service bit yourself.

```c
/* EOI is a 32-bit write to APIC offset 0x0B0 */
if (mmio_phys == 0xFEE000B0) {
    /* top of ISR is the vector to complete */
    uint32_t isr = *(uint32_t *)&lapic.regs[0x100];
    uint8_t vec = __builtin_ctz(isr);
    clear_isr_bit(vec);
}
```

---

## 11. Putting it together: an emulated serial port

A common pattern for an in-kernel PIC with a userspace device:

```c
/* 1. create the interrupt infrastructure */
ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0);

/* 2. set up a thread or event source for the serial port */
void *serial_thread(void *arg) {
    int ev = eventfd(0, EFD_CLOEXEC);
    struct kvm_irqfd irqfd = { .fd = ev, .gsi = 4 };
    ioctl(vm_fd, KVM_IRQFD, &irqfd);

    while (1) {
        wait_for_char();
        uint64_t one = 1;
        write(ev, &one, sizeof(one));   /* injects COM1 (GSI 4) */
    }
}
```

The guest's COM1 driver is interrupted, reads the data via `KVM_EXIT_IO`, and the PIC/I/O APIC is automatically cleared by the in-kernel code when the guest sends an EOI.

---

## 12. Quick reference: which ioctl for which case

| Use case | Ioctl |
|----------|-------|
| Create 8259 + I/O APIC | `KVM_CREATE_IRQCHIP` |
| Raise/clear a GSI line | `KVM_IRQ_LINE` |
| Legacy CPU-local vector (real mode) | `KVM_INTERRUPT` |
| Read/write local APIC | `KVM_GET_LAPIC` / `KVM_SET_LAPIC` |
| Send an MSI/MSI-X | `KVM_SIGNAL_MSI` |
| Eventfd-based GSI | `KVM_IRQFD` |
| Eventfd-based exit avoidance | `KVM_IOEVENTFD` |
| NMI | `KVM_NMI` |
| Exception | `KVM_SET_VCPU_EVENTS` |
| vCPU state for AP startup | `KVM_SET_MP_STATE` / `KVM_GET_MP_STATE` |

---

## 13. Further reading

* `Documentation/virt/kvm/api.rst` in the Linux tree (modern, thorough API reference).
* Intel SDM, Volume 3A, Chapters 10–10.10 (Local APIC).
* Intel SDM, Volume 3A, Chapter 11 (APIC bus, I/O APIC).
* `include/uapi/linux/kvm.h` in the Linux sources — all `struct` and `ioctl` definitions.
* QEMU source: `hw/intc/apic.c` and `hw/intc/ioapic2.c` for working in-kernel alternatives.
