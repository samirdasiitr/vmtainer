# 03 — VM Exits and the `kvm_run` Structure

## 1. What Is a VM Exit?

When a vCPU is running, the CPU is in **non-root mode** (Intel VMX) or **guest mode** (AMD SVM). When the guest attempts an operation that the VMM must handle — an I/O port access, a privileged instruction, an unknown address, an exception, or a halt — the CPU traps back to the kernel. This event is a **VM exit**. KVM records the reason and data in the per-vCPU `struct kvm_run`, then returns from `KVM_RUN` to userspace. The VMM then inspects `run->exit_reason` and takes action.

```
User VMM          Kernel KVM          Hardware (VT-x/SVM)
   |                   |                       |
   |  ioctl(KVM_RUN)   |                       |
   |------------------>| enter non-root mode   |
   |                   |---------------------->|
   |                   |   guest runs          |
   |                   |<----------------------|
   |                   |  VM exit              |
   |  return with      |                       |
   |  kvm_run filled   |                       |
   |<------------------|                       |
```

The VMM is essentially an **event loop driven by exits**. Efficiently handling exits is one of the most important aspects of VMM performance.

## 2. The `kvm_run` Structure

`struct kvm_run` is a shared memory page between the kernel and the VMM. It is mapped once per vCPU and is both input and output:

- **Input**: before `KVM_RUN`, the VMM can set `request_interrupt_window`, `immediate_exit`, and related fields.
- **Output**: after `KVM_RUN`, the VMM reads `exit_reason` and the per-exit union.

The key fields are (from `<linux/kvm.h>`):

```c
struct kvm_run {
    __u8  request_interrupt_window;
    __u8  immediate_exit;
    __u8  padding1[6];

    __u64 exit_reason;
    __u8  ready_for_interrupt_injection;
    __u8  if_flag;
    __u16 flags;
    __u64 cr8;
    __u64 apic_base;

    union {
        /* KVM_EXIT_IO */
        struct kvm_sync_regs *regs; /* not used directly */
    };

    __u64 padding2[120];

    union {
        /* ... */
        struct {
            __u64 hardware_entry_failure_reason;
        } fail_entry;
        struct {
            __u32 suberror;
            /* ... */
        } internal_error;
        struct {
            __u8  direction;
            __u8  size;
            __u16 port;
            __u32 count;
            __u64 data_offset;
        } io;
        struct {
            __u8  is_write;
            __u64 phys_addr;
            __u8  data[8];
            __u32 len;
        } mmio;
        /* ... and many more ... */
    } s;
};
```

Use the kernel header; do not copy the structure manually. The exact layout depends on the host architecture and kernel version.

## 3. The Dispatch Loop

A typical dispatcher looks like this:

```c
int run_vcpu(int vcpu_fd, struct kvm_run *run)
{
    for (;;) {
        int r = ioctl(vcpu_fd, KVM_RUN, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) continue;
            perror("KVM_RUN");
            return -1;
        }

        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            handle_hlt(run);
            break;
        case KVM_EXIT_IO:
            handle_io(run);
            break;
        case KVM_EXIT_MMIO:
            handle_mmio(run);
            break;
        case KVM_EXIT_MSR:
            handle_msr(run);
            break;
        case KVM_EXIT_CPUID:
            handle_cpuid(run);
            break;
        case KVM_EXIT_IOAPIC_EOI:
            handle_eoi(run);
            break;
        case KVM_EXIT_SHUTDOWN:
            return 0;    /* normal VM shutdown */
        case KVM_EXIT_FAIL_ENTRY:
            report_fail_entry(run);
            return -1;
        case KVM_EXIT_INTERNAL_ERROR:
            report_internal_error(run);
            return -1;
        default:
            fprintf(stderr, "unhandled exit %llu\n",
                    (unsigned long long)run->exit_reason);
            return -1;
        }
    }
}
```

## 4. Exit Reasons in Detail

### 4.1 `KVM_EXIT_HLT`

The guest executed `HLT`, `MWAIT`, or another halt-like instruction. The vCPU has stopped until the next interrupt. In a single-vCPU toy VMM you might simply stop the machine. In a real VMM:

- Mark the vCPU as halted.
- If the guest's interrupt flag (`IF`) is set, wait on a condition variable for an `ioeventfd`, `irqfd`, `KVM_INTERRUPT`, or `KVM_SET_MP_STATE`.
- Resume with `KVM_RUN` after injecting an interrupt.

```c
void handle_hlt(struct kvm_run *run)
{
    if (run->if_flag) {
        /* Guest can receive interrupts; wait for one. */
        vcpu_halted = 1;
    } else {
        /* IF=0 and HLT: the vCPU will never wake up without an NMI or SMI. */
        fprintf(stderr, "Halt with IF=0\n");
    }
}
```

### 4.2 `KVM_EXIT_IO`

The guest executed `IN` or `OUT`. The `run->io` substructure contains:

- `port` — the I/O port.
- `size` — 1, 2, or 4 bytes.
- `direction` — `KVM_EXIT_IO_IN` or `KVM_EXIT_IO_OUT`.
- `count` — number of repetitions (for `REP INS`/`OUTS`).
- `data_offset` — offset from the start of the `kvm_run` mapping where data is stored.

```c
void handle_io(struct kvm_run *run)
{
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    uint16_t port = run->io.port;
    uint8_t size = run->io.size;
    uint32_t count = run->io.count;

    for (uint32_t i = 0; i < count; i++) {
        if (run->io.direction == KVM_EXIT_IO_OUT) {
            uint32_t value = 0;
            if (size == 1) value = data[0];
            else if (size == 2) value = *(uint16_t *)data;
            else if (size == 4) value = *(uint32_t *)data;

            port_out(port, value, size);
        } else {
            uint32_t value = port_in(port, size);
            if (size == 1) data[0] = value;
            else if (size == 2) *(uint16_t *)data = value;
            else if (size == 4) *(uint32_t *)data = value;
        }
        data += size;
    }
}
```

The data is in the same page as `kvm_run`, at `run + data_offset`. Do not read or write beyond the page.

### 4.3 `KVM_EXIT_MMIO`

The guest accessed a physical address that is **not backed by RAM**. This is used for memory-mapped devices.

```c
void handle_mmio(struct kvm_run *run)
{
    uint64_t addr = run->mmio.phys_addr;
    uint32_t len = run->mmio.len;
    bool is_write = run->mmio.is_write;

    if (is_write) {
        mmio_write(addr, run->mmio.data, len);
    } else {
        mmio_read(addr, run->mmio.data, len);
    }
}
```

For small devices, in-userspace MMIO is fine. For high-throughput devices, use `ioeventfd` or `irqfd` to avoid the exit.

### 4.4 `KVM_EXIT_MSR`

The guest executed `RDMSR` or `WRMSR` on a model-specific register not handled in-kernel. The `run->msr` substructure gives the MSR index, the value, and whether it is a read or write.

```c
void handle_msr(struct kvm_run *run)
{
    uint32_t index = run->msr.index;
    uint64_t value = run->msr.data;

    if (run->msr.write) {
        msr_write(index, value);
    } else {
        run->msr.data = msr_read(index);
    }
}
```

Many MSRs (e.g., `MSR_IA32_EFER`, `MSR_IA32_APICBASE`) are managed by KVM, but hypervisor-specific or emulated MSRs can be handled here.

### 4.5 `KVM_EXIT_CPUID`

The guest executed `CPUID`. KVM can be configured to handle `CPUID` in userspace with `KVM_SET_CPUID2` and the `KVM_CPUID_FLAG_SIGNEXT`/`KVM_CPUID_FLAG_CHECK_SUBLEAVES` flags. When `KVM_EXIT_CPUID` occurs, `run->cpuid` provides the leaf, subleaf, and output.

In practice it is much better to set a CPUID table once with `KVM_SET_CPUID2` and let the kernel respond. A userspace handler is only needed if the VMM wants to expose dynamic leaves such as `KVM_CPUID_FEATURES` (the KVM paravirtual wall clock or steal time leaves).

### 4.6 `KVM_EXIT_SYSTEM_EVENT` / `KVM_EXIT_SHUTDOWN`

`KVM_EXIT_SHUTDOWN` means the guest has executed a triple fault, a `CLI`+`HLT` with no way out, or `KVM_SHUTDOWN` was triggered. `KVM_EXIT_SYSTEM_EVENT` is newer and more explicit; it contains a `type` field such as:

- `KVM_SYSTEM_EVENT_SHUTDOWN`
- `KVM_SYSTEM_EVENT_RESET`
- `KVM_SYSTEM_EVENT_CRASH`

```c
if (run->exit_reason == KVM_EXIT_SYSTEM_EVENT) {
    if (run->system_event.type == KVM_SYSTEM_EVENT_SHUTDOWN)
        return VM_SHUTDOWN;
    if (run->system_event.type == KVM_SYSTEM_EVENT_RESET)
        return VM_RESET;
}
```

### 4.7 `KVM_EXIT_DEBUG`

The guest hit a debug breakpoint, single-step, or hardware debug event. To receive this exit, set up `KVM_SET_GUEST_DEBUG` with `KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP` or enable hardware breakpoints.

```c
struct kvm_guest_debug dbg = {
    .control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP,
};
ioctl(vcpu_fd, KVM_SET_GUEST_DEBUG, &dbg);
```

### 4.8 `KVM_EXIT_IRQ_WINDOW_OPEN`

When the VMM sets `run->request_interrupt_window = 1`, the next `KVM_RUN` returns with `KVM_EXIT_IRQ_WINDOW_OPEN` when the guest is in a state where an interrupt can be injected. This is how you deliver interrupts asynchronously.

```c
void request_injection(int vcpu_fd, struct kvm_run *run)
{
    if (run->ready_for_interrupt_injection) {
        inject_interrupt(vcpu_fd, vector);
    } else {
        run->request_interrupt_window = 1;
    }
}
```

### 4.9 `KVM_EXIT_HYPERCALL` and `KVM_EXIT_DEBUG`

- `KVM_EXIT_HYPERCALL`: the guest executed `VMCALL` (Intel) or `VMMCALL` (AMD) under some conditions. You can define custom hypercalls for paravirtualization, but modern KVM uses `KVM_HC_` via `KVM_EXIT_HYPERCALL` or the `KVM_ASYNC_PF`/`KVM_CLOCK` subsystems.
- `KVM_EXIT_NMI`: a non-maskable interrupt was processed.

### 4.10 `KVM_EXIT_FAIL_ENTRY`

A hardware-specific failure occurred while entering non-root mode. The `hardware_entry_failure_reason` field gives the raw error code from the VMCS or VMCB. Common causes:

- Invalid `cr0`, `cr4`, or `efer`.
- PAE not enabled while long mode is requested.
- A non-canonical `rip` or `rsp`.
- A missing `VMX controls` MSR.

### 4.11 `KVM_EXIT_INTERNAL_ERROR`

KVM detected an internal consistency problem. The `suberror` field tells you what kind:

- `KVM_INTERNAL_ERROR_EMULATION` — the VMM configured userspace I/O and the access could not be emulated.
- `KVM_INTERNAL_ERROR_SIMUL_EX` — a simulated exception during instruction emulation.
- `KVM_INTERNAL_ERROR_DELIVERY_EV` — an exception occurred while delivering an event.

These usually indicate a bug in guest state, memory layout, or device emulation.

## 5. Putting It Together: An Exit Dispatch Table

```c
typedef void (*exit_handler_t)(struct kvm_run *, void *);

void dispatch_exit(int vcpu_fd, struct kvm_run *run, void *ctx)
{
    static exit_handler_t handlers[] = {
        [KVM_EXIT_HLT]          = handle_hlt,
        [KVM_EXIT_IO]           = handle_io,
        [KVM_EXIT_MMIO]         = handle_mmio,
        [KVM_EXIT_MSR]          = handle_msr,
        [KVM_EXIT_CPUID]        = handle_cpuid,
        [KVM_EXIT_SHUTDOWN]     = handle_shutdown,
        [KVM_EXIT_SYSTEM_EVENT] = handle_system_event,
        [KVM_EXIT_DEBUG]        = handle_debug,
        [KVM_EXIT_IOAPIC_EOI]   = handle_ioapic_eoi,
    };

    if (run->exit_reason < sizeof(handlers)/sizeof(handlers[0])
        && handlers[run->exit_reason])
    {
        handlers[run->exit_reason](run, ctx);
    } else {
        errx(1, "Unhandled exit %llu", (unsigned long long)run->exit_reason);
    }
}
```

## 6. Summary

| Exit reason | Typical cause | What the VMM does |
|-------------|---------------|-------------------|
| `KVM_EXIT_IO` | `IN`/`OUT` to unaccelerated port | Emulate device, set `data` for `IN` |
| `KVM_EXIT_MMIO` | Access to unmapped GPA | Emulate MMIO device |
| `KVM_EXIT_HLT` | `HLT` | Wait for an interrupt |
| `KVM_EXIT_MSR` | `RDMSR`/`WRMSR` to an emulated MSR | Read or write the MSR |
| `KVM_EXIT_CPUID` | `CPUID` with userspace filtering | Fill in EAX..EDX |
| `KVM_EXIT_SHUTDOWN` | Triple fault / `CLI HLT` | Terminate or reset |
| `KVM_EXIT_FAIL_ENTRY` | Bad guest state / VMCS error | Abort and debug initial state |
| `KVM_EXIT_INTERNAL_ERROR` | Kernel emulation problem | Inspect `suberror` and abort |
| `KVM_EXIT_IRQ_WINDOW_OPEN` | Interrupt can now be injected | Inject the queued interrupt |

The VMM run loop is essentially a `switch` over these reasons. In the next chapter we implement actual device handlers for `KVM_EXIT_IO` and `KVM_EXIT_MMIO`.
