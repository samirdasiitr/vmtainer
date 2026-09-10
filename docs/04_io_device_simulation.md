# 04 — I/O Device Simulation

## 1. Why Simulate Devices?

A VMM must provide the guest with the illusion of real hardware: serial ports, PIC, PIT, RTC, disk, network, and PCI. There are three techniques to implement this:

1. **Port I/O emulation** — the guest runs `IN`/`OUT` instructions, which cause `KVM_EXIT_IO`. The VMM reads or writes the data.
2. **MMIO emulation** — the guest touches a physical address not in RAM. KVM raises `KVM_EXIT_MMIO`. The VMM emulates the device.
3. **KVM accelerators** — `ioeventfd` and `irqfd` reduce exits by moving notifications into the kernel, and the in-kernel PIC/APIC/PIT/IO-APIC handle common cases.

This chapter covers port I/O, MMIO, the in-kernel interrupt controllers, `ioeventfd`/`irqfd`, and a simple emulated serial port.

## 2. Port I/O Emulation

### Detecting the access

`KVM_EXIT_IO` is raised when the guest executes `IN`, `INS`, `OUT`, or `OUTS` to a port that is not handled in-kernel. The `run->io` substructure gives the port, size, direction, count, and data offset.

### Example: emulate a `0x80` debug port

Port `0x80` is commonly used as a POST code / debug port by firmware. Writing a byte to it is a cheap way to log guest progress.

```c
void handle_debug_port(struct kvm_run *run)
{
    if (run->io.port != 0x80 || run->io.size != 1)
        return;

    uint8_t *data = (uint8_t *)run + run->io.data_offset;

    if (run->io.direction == KVM_EXIT_IO_OUT) {
        printf("POST 0x%02x\n", data[0]);
    } else {
        data[0] = 0x00;  /* reading 0x80 is not meaningful */
    }
}
```

### Example: 8255 / PC speaker port B (0x61)

The speaker port is commonly used by the BIOS. A minimal VMM can log it or return zero:

```c
void handle_speaker_port(struct kvm_run *run)
{
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    if (run->io.direction == KVM_EXIT_IO_OUT) {
        speaker_state = data[0];
    } else {
        data[0] = speaker_state;
    }
}
```

### Handling `INS`/`OUTS` (string I/O)

When `run->io.count > 1` or the direction is a string operation, the data pointer already contains the whole buffer. The VMM loops `count` times, advancing `data` by `size`.

```c
uint8_t *ptr = (uint8_t *)run + run->io.data_offset;
for (uint32_t i = 0; i < run->io.count; i++) {
    if (run->io.direction == KVM_EXIT_IO_OUT) {
        port_write(run->io.port, ptr, run->io.size);
    } else {
        port_read(run->io.port, ptr, run->io.size);
    }
    ptr += run->io.size;
}
```

## 3. MMIO Emulation

### Detecting the access

When the guest reads or writes a guest-physical address that falls outside all registered memory slots, or that is intentionally marked as a device region, KVM raises `KVM_EXIT_MMIO`. The `run->mmio` substructure gives the address, length, direction, and `data[8]`.

### Example: emulated power-off button at GPA 0x400

```c
void handle_poweroff_mmio(struct kvm_run *run)
{
    if (run->mmio.is_write) {
        if (run->mmio.phys_addr == 0x400 && run->mmio.len == 1) {
            if (run->mmio.data[0] == 0x01) {
                vm_running = 0;
                printf("Guest requested power off\n");
            }
        }
    } else {
        /* Read returns the current power state. */
        run->mmio.data[0] = vm_running ? 0x00 : 0x01;
        memset(run->mmio.data + 1, 0, run->mmio.len - 1);
    }
}
```

### Example: emulated PCI configuration space at 0xc0000

A minimal PCI host bridge forwards `CF8`/`CFC` accesses to memory. A real VMM would decode the `CF8` register, locate the target device, and read or write the device's config space. For now, the pattern is the same: the VMM maintains state and copies bytes through `run->mmio.data`.

```c
uint8_t cfg[256] = {0};

void emulate_pci_cfg_read(uint64_t addr, uint8_t *out, uint32_t len)
{
    for (uint32_t i = 0; i < len && i < 8; i++)
        out[i] = cfg[(addr + i) % sizeof(cfg)];
}

void emulate_pci_cfg_write(uint64_t addr, const uint8_t *in, uint32_t len)
{
    for (uint32_t i = 0; i < len && i < 8; i++)
        cfg[(addr + i) % sizeof(cfg)] = in[i];
}
```

## 4. The In-Kernel PIC, IO-APIC, and Local APIC

### Creating the in-kernel interrupt controllers

```c
ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0);
```

This one call creates the virtual 8259A PIC, virtual IO-APIC, and virtual local APIC per vCPU. Using the in-kernel APIC is almost always faster and simpler than emulating APIC in userspace.

### Legacy PIC (8259A)

The 8259A has a master at base `0x20` and a slave at base `0xa0`. KVM emulates these in the kernel. To request an interrupt for a vCPU, use `KVM_INTERRUPT` with an IRQ vector number:

```c
struct kvm_interrupt intr = { .irq = 0x20 };  /* timer vector */
ioctl(vcpu_fd, KVM_INTERRUPT, &intr);
```

For level-triggered or EOI-based handling, `KVM_CREATE_IRQCHIP` plus `KVM_SET_IRQ_LINE` is more appropriate.

### IO-APIC

The virtual IO-APIC redirects external IRQs to vCPUs. By default, when the VMM raises `KVM_SET_IRQ_LINE`, the in-kernel IO-APIC delivers the interrupt to a vCPU according to the IO-APIC redirection entries. The VMM does not need to manage APIC IDs manually unless it is doing explicit routing.

### Local APIC

Each vCPU has its own local APIC. See Chapter 5 for details. In this chapter, the important point is that `KVM_CREATE_IRQCHIP` must be called before `KVM_CREATE_VCPU` if the vCPUs are to be created with a local APIC.

## 5. `ioeventfd`: Accelerating I/O Writes

`ioeventfd` lets a guest I/O access directly signal an `eventfd` in the VMM process without exiting to userspace. This is extremely useful for high-throughput virtual devices such as virtio.

### How it works

1. The VMM creates an `eventfd` with `eventfd(0, EFD_NONBLOCK)`.
2. The VMM registers a region of guest memory or an I/O port with `KVM_IOEVENTFD`.
3. When the guest writes the configured value to the configured address, the kernel increments the `eventfd`.
4. A VMM thread waits on `poll`/`epoll` on the `eventfd` and performs device work in a separate context.

```c
#include <sys/eventfd.h>

int efd = eventfd(0, EFD_NONBLOCK);
if (efd < 0) err(1, "eventfd");

struct kvm_ioeventfd ioe = {
    .datamatch = 1,
    .fd = efd,
    .flags = 0,
    .addr = 0x1000,        /* guest physical address or port */
    .len = 1,
    .addr_type = KVM_IOEVENTFD_FLAG_PIO,  /* or 0 for MMIO */
};

ioctl(vm_fd, KVM_IOEVENTFD, &ioe);
```

Use `KVM_IOEVENTFD_FLAG_DEASSIGN` to remove the registration. Multiple `ioeventfd`s can be registered, each with a different address or value.

## 6. `irqfd`: Injecting Interrupts from Userspace

`irqfd` is the complement of `ioeventfd`. The VMM registers an `eventfd`, and whenever the `eventfd` is written, the kernel injects an interrupt into a vCPU.

```c
int irq_eventfd = eventfd(0, EFD_NONBLOCK);

struct kvm_irqfd irqfd = {
    .fd = irq_eventfd,
    .flags = 0,
    .gsi = 4,            /* the guest IRQ number, e.g., COM1 */
    .resamplefd = -1,
};

ioctl(vm_fd, KVM_IRQFD, &irqfd);
```

Now any device thread that writes a 64-bit `1` to `irq_eventfd` will cause KVM to inject the configured `gsi` into the VM. The `gsi` is the "global system interrupt" that the in-kernel IO-APIC routes to a vCPU.

This is how a paravirtualized block driver works: a kernel worker in the guest does `outl` to an `ioeventfd`-registered port to signal the host, and the host thread then signals back via `irqfd` when the request is complete.

## 7. Example: A Simple Emulated Serial Port

A minimal 16550A-compatible serial port is one of the most useful devices for a toy VMM. It lets you see boot messages, get a BIOS shell, and run a console.

### Register layout

```
0x3f8: RBR/THR   (read: receiver buffer, write: transmit hold)
0x3f9: IER       (interrupt enable)
0x3fa: IIR       (read: interrupt identification)
0x3fb: LCR       (line control)
0x3fc: MCR       (modem control)
0x3fd: LSR       (line status)
0x3fe: MSR       (modem status)
0x3ff: SCR       (scratch)
```

### Emulation skeleton

```c
struct serial_state {
    uint8_t thr;    /* 0x3f8 */
    uint8_t ier;    /* 0x3f9 */
    uint8_t iir;    /* 0x3fa */
    uint8_t lcr;    /* 0x3fb */
    uint8_t mcr;    /* 0x3fc */
    uint8_t lsr;    /* 0x3fd */
    uint8_t msr;    /* 0x3fe */
    uint8_t scr;    /* 0x3ff */
};

static struct serial_state com1 = { .lsr = 0x60 };

void handle_serial(uint16_t port, int size, int dir, uint8_t *data)
{
    uint16_t reg = port - 0x3f8;
    if (reg > 7) return;

    if (dir == KVM_EXIT_IO_OUT) {
        switch (reg) {
        case 0:
            if (com1.lcr & 0x80) {  /* DLAB set */
                /* divisor latch low */
            } else {
                putchar(data[0]);   /* print guest character */
                fflush(stdout);
            }
            break;
        case 1:
            if (com1.lcr & 0x80) {  /* DLAB: divisor latch high */
            } else {
                com1.ier = data[0];
            }
            break;
        case 2: com1.iir  = data[0]; break;
        case 3: com1.lcr  = data[0]; break;
        case 4: com1.mcr  = data[0]; break;
        case 5: com1.lsr  = data[0]; break;
        case 6: com1.msr  = data[0]; break;
        case 7: com1.scr  = data[0]; break;
        }
    } else {  /* IN */
        switch (reg) {
        case 0:
            if (com1.lcr & 0x80) data[0] = 0;
            else data[0] = 0;  /* no input */
            break;
        case 1: data[0] = com1.ier; break;
        case 2: data[0] = 0x01; break;  /* no pending interrupts */
        case 3: data[0] = com1.lcr; break;
        case 4: data[0] = com1.mcr; break;
        case 5: data[0] = 0x60; break;  /* THR empty, no error */
        case 6: data[0] = 0x00; break;
        case 7: data[0] = 0x00; break;
        }
    }
}
```

Register the 8 sequential ports `0x3f8..0x3ff`. When a `KVM_EXIT_IO` occurs in that range, call `handle_serial`. This is enough for many bootloaders and for the Linux early console.

### Routing a serial interrupt

A 16550A raises IRQ 4. When the guest reads `0x3f8` (RBR) or when THR becomes empty, the device should raise IRQ 4. With the in-kernel PIC:

```c
void serial_raise_irq(int vcpu_fd)
{
    if (com1.ier & 0x02) {  /* THRE interrupt enabled */
        struct kvm_interrupt intr = { .irq = 4 };
        ioctl(vcpu_fd, KVM_INTERRUPT, &intr);
    }
}
```

For a more realistic implementation, use `ioeventfd` for the THR write and `irqfd` to signal the completion.

## 8. Example: A Minimal RTC (MC146818)

The x86 CMOS RTC at ports `0x70` and `0x71` is accessed by selecting a register index with `0x70` and reading or writing data at `0x71`.

```c
uint8_t cmos_index = 0;
uint8_t cmos_data[128] = {0};

void handle_cmos(uint16_t port, int size, int dir, uint8_t *data)
{
    if (port == 0x70 && dir == KVM_EXIT_IO_OUT) {
        cmos_index = data[0] & 0x7f;
    } else if (port == 0x71) {
        if (dir == KVM_EXIT_IO_OUT) {
            cmos_data[cmos_index] = data[0];
        } else {
            data[0] = cmos_data[cmos_index];
        }
    }
}
```

For a real Linux guest, you must provide the **century** and **memory size** in specific registers. The most important for early boot are:

- Register `0x14..0x17` and `0x18..0x1b` — base and extended memory in KB.
- Register `0x32` — century.
- Register `0x0b` — status register B (data format).

Set these before booting so the BIOS or kernel gets the right memory size and time format.

## 9. Interrupt Injection

### Legacy `KVM_INTERRUPT`

For a vCPU that uses the in-kernel PIC and has `IF=1`:

```c
struct kvm_interrupt intr = { .irq = 4 };
ioctl(vcpu_fd, KVM_INTERRUPT, &intr);
```

### `KVM_SET_IRQ_LINE`

For level or edge-triggered interrupts tied to the in-kernel IO-APIC:

```c
struct kvm_irq_level lvl = {
    .irq = 4,
    .level = 1,   /* 1 = assert, 0 = de-assert */
};
ioctl(vm_fd, KVM_SET_IRQ_LINE, &lvl);
```

For level-triggered interrupts, remember to de-assert the line after the EOI. For edge-triggered (most legacy ISA devices), set `level = 0` immediately after `level = 1`.

## 10. Summary

- Port I/O and MMIO exits drive userspace device emulation.
- The in-kernel `KVM_CREATE_IRQCHIP` provides PIC, IO-APIC, and local APIC.
- `ioeventfd` and `irqfd` move fast-path notifications out of the `KVM_RUN` loop.
- A simple 16550A serial port and an MC146818 RTC are the two most useful devices to implement first.
- Interrupts can be injected with `KVM_INTERRUPT`, `KVM_SET_IRQ_LINE`, or `irqfd`.
