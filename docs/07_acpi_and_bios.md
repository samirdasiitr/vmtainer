# 07 — BIOS, Firmware, and ACPI Tables

## 1. Why Firmware and ACPI Matter

A VMM does not just run a kernel. It must boot an operating system. On x86 that usually means providing:

1. **Firmware** (BIOS/UEFI) — a small binary that runs first in 16-bit real mode, resets devices, and loads the operating system.
2. **ACPI tables** — data structures that tell the OS about processors, memory, interrupts, power management, and devices.
3. A **guest physical memory layout** — the map of RAM, firmware, devices, and special regions.

This chapter explains the minimal firmware + ACPI setup needed to boot a Linux kernel in a simple VMM.

## 2. BIOS and Firmware

### Bochs BIOS

The easiest firmware for a from-scratch VMM is the **Bochs BIOS** (`BIOS-bochs-latest`) or the **SeaBIOS** (`bios.bin`) normally shipped with QEMU. Both are open source and expect a standard PC environment:

- 16-bit real-mode boot at `CS:IP = 0xf000:0xfff0` (physical `0xffff0`).
- CMOS at `0x70/0x71`.
- 8259 PIC at `0x20/0xa0`.
- 8254 PIT at `0x40`.
- 8042 keyboard / PS/2 controller.
- VGA text video memory at `0xb8000`.
- A bootable hard disk, CD-ROM, or a Linux bzImage.

### Minimal "firmware" you can write yourself

For a very small VMM that loads a Linux kernel directly, you can avoid a full BIOS. You just place a tiny 16-bit boot stub at `0xffff0` that jumps to the Linux boot sector. However, most operating systems need at least a fake ACPI environment.

### Loading the BIOS into guest memory

The BIOS ROM is typically placed at the top of the 4 GiB address space. On a 32-bit PC, this is `0xfffc0000` to `0xffffffff` or the last 64 KiB at `0xf0000` to `0xfffff`. Bochs BIOS is often loaded at the end of the 4 GiB space.

```c
void *load_bios(void *guest_mem, size_t guest_size, const char *path)
{
    size_t bios_size = file_size(path);
    uint8_t *bios = malloc(bios_size);
    read_file(path, bios, bios_size);

    /* Place at the top of the 32-bit address space. */
    size_t gpa = 0x100000000ULL - bios_size;
    if (gpa < guest_size) {
        memcpy((uint8_t *)guest_mem + gpa, bios, bios_size);
    }
    free(bios);
    return (uint8_t *)guest_mem + gpa;
}
```

Set the vCPU initial `rip` to the reset vector. For a BIOS at `0xfffc0000` with a reset vector at `0xfffffff0`:

```c
sregs.cs.selector = 0xf000;
sregs.cs.base = 0xffff0000;
regs.rip = 0xfff0;
```

For a smaller 64 KiB ROM at `0xf0000`:

```c
sregs.cs.selector = 0xf000;
sregs.cs.base = 0xf0000;
regs.rip = 0xfff0;  /* physical 0xffff0 */
```

## 3. The Shutdown Port

A simple convention used by Bochs/SeaBIOS and many VMMs is the **ACPI shutdown port** at I/O port `0x4004` (QEMU convention `0x604`) or another chosen port. When the guest writes a magic value, the VMM stops the VM.

```c
case KVM_EXIT_IO:
    if (run->io.port == 0x4004 && run->io.direction == KVM_EXIT_IO_OUT) {
        uint8_t *data = (uint8_t *)run + run->io.data_offset;
        if (data[0] == 0x01) {
            vm_running = 0;
            return 0;
        }
    }
```

The exact port is up to the VMM; the ACPI `PM1a_CNT` port is the real one, but a magic debug port works for testing.

## 4. Guest Physical Memory Layout

A typical layout for a small x86_64 guest with 128 MiB RAM:

```
0x00000000 - 0x0009ffff      Low 640 KiB
0x000a0000 - 0x000effff      VGA / device areas
0x000f0000 - 0x000fffff      64 KiB BIOS (or other firmware)
0x00100000 - 0x...           Kernel and initrd
0x...                        Free memory
0xb8000000 - 0xb8007fff      Optional high-memory / PCI hole
0xe0000000 - 0xefffffff      PCI device MMIO region
0xf0000000 - 0xf0ffffff      Optional PCI config / mmconfig
0xfee00000                   Local APIC xAPIC page
0xfec00000                   IO-APIC
0xfffc0000 - 0xffffffff      BIOS ROM / reset vector
```

In a 64-bit guest the physical address space can be much larger. The RAM can go up to 1 GiB, 4 GiB, or more, with a PCI hole above the RAM.

## 5. Introduction to ACPI

**ACPI (Advanced Configuration and Power Interface)** describes the platform to the OS. The OS locates ACPI by:

1. Looking for the **RSDP (Root System Description Pointer)** in low memory (`0x000e0000` to `0x000fffff` or `0x00080000` to `0x0009ffff`) or the high ROM region.
2. Reading the RSDP to find the **RSDT** (32-bit) or **XSDT** (64-bit).
3. Following the RSDT/XSDT to tables like the **MADT**, **FADT**, **DSDT**, and **SSDT**.

For a minimal VMM, the RSDP, RSDT, MADT, FADT, and a tiny DSDT are enough.

## 6. Building the RSDP

The RSDP has two versions: v1 (20 bytes) and v2+ (36 bytes). Version 2 is for 64-bit XSDT.

```c
struct acpi_rsdp {
    char     signature[8];    /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;        /* 0 = v1, 2 = v2 */
    uint32_t rsdt_address;
    uint32_t length;          /* v2 only */
    uint64_t xsdt_address;    /* v2 only */
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));
```

Checksum: sum of all bytes of the v1 portion must be 0. Extended checksum: sum of all 36 bytes must be 0.

Place the RSDP on a 16-byte boundary in low memory, e.g., `0x000e0000`.

## 7. Building the RSDT

```c
struct acpi_table_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} __attribute__((packed));

struct rsdt {
    struct acpi_table_header h;
    uint32_t tables[];
} __attribute__((packed));
```

`RSDT` is an array of 32-bit physical addresses to other tables. The `length` includes the header plus the array. Checksum covers the whole table.

## 8. Building the MADT (APIC table)

The MADT (`APIC`) describes processors, I/O-APICs, interrupt overrides, and NMI sources. It contains a header and a list of variable-length entries.

```c
struct madt {
    struct acpi_table_header h;
    uint32_t lapic_base;     /* 0xfee00000 */
    uint32_t flags;          /* bit 0: dual 8259 */
    uint8_t  entries[];
} __attribute__((packed));
```

Entry types:

- `0` — processor local APIC
- `1` — I/O APIC
- `2` — interrupt source override
- `3` — NMI source
- `4` — local APIC NMI
- `5` — local APIC address override (64-bit)

### Processor local APIC entry

```c
struct madt_lapic {
    uint8_t  type;    /* 0 */
    uint8_t  length;  /* 8 */
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;   /* bit 0: enabled */
} __attribute__((packed));
```

For one vCPU:

```c
struct madt_lapic e = {
    .type = 0,
    .length = sizeof(e),
    .acpi_processor_id = 0,
    .apic_id = 0,
    .flags = 1,  /* enabled */
};
```

### I/O APIC entry

```c
struct madt_ioapic {
    uint8_t  type;    /* 1 */
    uint8_t  length;  /* 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;  /* 0xfec00000 */
    uint32_t gsi_base;     /* 0 */
} __attribute__((packed));
```

### Interrupt source override

```c
struct madt_iso {
    uint8_t  type;    /* 2 */
    uint8_t  length;  /* 10 */
    uint8_t  bus;     /* 0 = ISA */
    uint8_t  source;  /* ISA IRQ */
    uint32_t gsi;     /* IO-APIC input */
    uint16_t flags;   /* polarity/trigger */
} __attribute__((packed));
```

For a typical PC:

- IRQ 0 -> GSI 2 (timer)
- IRQ 1 -> GSI 1 (keyboard)
- IRQ 4 -> GSI 4 (COM1)
- IRQ 8 -> GSI 8 (RTC)

## 9. Building the FADT

The FADT (`FACP`) is one of the most important tables. It points to the DSDT and provides power management and I/O ports.

A minimal FADT:

```c
struct fadt {
    struct acpi_table_header h;
    uint32_t facs;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evnt_blk;
    uint32_t pm1b_evnt_blk;
    uint32_t pm1a_cnt_blk;   /* e.g., 0x4004 */
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evnt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_len;
    uint8_t  gpe1_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t boot_arch_flags;
    uint8_t  reserved2;
    uint32_t flags;
    /* 64-bit fields may follow; keep it small. */
} __attribute__((packed));
```

`dsdt` is the 32-bit physical address of the DSDT.

## 10. Building the DSDT

The DSDT is an AML (ACPI Machine Language) bytecode table. It describes devices and power states. Writing AML by hand is tedious. For a minimal VMM, you can use a precompiled, tiny DSDT that contains:

- A `_SB` system bus.
- A `PWRB` (power button) device.
- A `SLPB` (sleep button) device.
- An `S5_` sleep state package.

A minimal DSDT assembly (with `iasl`):

```asl
DefinitionBlock ("", "DSDT", 2, "VMM  ", "VMMDSDT ", 0x00000001)
{
    Name (_S5, Package (0x02)
    {
        0x05,
        0x05
    })

    Scope (\_SB)
    {
        Device (PWRB)
        {
            Name (_HID, EisaId ("PNP0C0C"))
        }

        Device (SLPB)
        {
            Name (_HID, EisaId ("PNP0C0E"))
        }
    }
}
```

Compile with:

```bash
iasl dsdt.asl
```

This produces `dsdt.aml`, which you load into guest memory and reference from the FADT.

## 11. Example: Writing the Tables to Guest Memory

```c
void build_acpi(void *guest_mem, size_t ram_size, int num_cpus,
                uint32_t rsdp_gpa, uint32_t rsdt_gpa,
                uint32_t madt_gpa, uint32_t fadt_gpa,
                uint32_t dsdt_gpa, size_t dsdt_len)
{
    struct acpi_rsdp rsdp = {0};
    memcpy(rsdp.signature, "RSD PTR ", 8);
    memcpy(rsdp.oem_id, "VMM   ", 6);
    rsdp.revision = 0;
    rsdp.rsdt_address = rsdt_gpa;
    rsdp.checksum = acpi_checksum(&rsdp, 20);
    acpi_write_guest(guest_mem, rsdp_gpa, &rsdp, 20);

    struct rsdt rsdt = {0};
    memcpy(rsdt.h.signature, "RSDT", 4);
    rsdt.h.length = sizeof(rsdt.h) + 3 * sizeof(uint32_t);
    rsdt.h.revision = 1;
    memcpy(rsdt.h.oem_id, "VMM   ", 6);
    rsdt.tables[0] = madt_gpa;
    rsdt.tables[1] = fadt_gpa;
    rsdt.h.checksum = acpi_checksum(&rsdt, rsdt.h.length);
    acpi_write_guest(guest_mem, rsdt_gpa, &rsdt, rsdt.h.length);

    /* MADT */
    size_t madt_size = sizeof(struct madt) + num_cpus * 8 + 12 + 4 * 10;
    struct madt *madt = calloc(1, madt_size);
    memcpy(madt->h.signature, "APIC", 4);
    madt->h.length = madt_size;
    madt->h.revision = 1;
    madt->lapic_base = 0xfee00000;
    madt->flags = 0;

    uint8_t *p = madt->entries;
    for (int i = 0; i < num_cpus; i++) {
        struct madt_lapic *lap = (struct madt_lapic *)p;
        lap->type = 0;
        lap->length = sizeof(*lap);
        lap->acpi_processor_id = i;
        lap->apic_id = i;
        lap->flags = 1;
        p += sizeof(*lap);
    }

    struct madt_ioapic *ioa = (struct madt_ioapic *)p;
    ioa->type = 1;
    ioa->length = sizeof(*ioa);
    ioa->ioapic_id = num_cpus;
    ioa->ioapic_addr = 0xfec00000;
    ioa->gsi_base = 0;
    p += sizeof(*ioa);

    /* Interrupt source override: IRQ0 -> GSI2 */
    struct madt_iso *iso = (struct madt_iso *)p;
    iso->type = 2;
    iso->length = sizeof(*iso);
    iso->bus = 0;
    iso->source = 0;
    iso->gsi = 2;
    iso->flags = 0;
    p += sizeof(*iso);

    madt->h.checksum = acpi_checksum(madt, madt_size);
    acpi_write_guest(guest_mem, madt_gpa, madt, madt_size);
    free(madt);

    /* FADT */
    struct fadt fadt = {0};
    memcpy(fadt.h.signature, "FACP", 4);
    fadt.h.length = sizeof(fadt);
    fadt.h.revision = 1;
    fadt.dsdt = dsdt_gpa;
    fadt.pm1a_cnt_blk = 0x4004;
    fadt.pm1_cnt_len = 2;
    fadt.boot_arch_flags = 0x0;  /* no legacy devices */
    fadt.h.checksum = acpi_checksum(&fadt, sizeof(fadt));
    acpi_write_guest(guest_mem, fadt_gpa, &fadt, sizeof(fadt));

    /* DSDT */
    /* Load the precompiled dsdt.aml from a file. */
}
```

## 12. Summary

- A BIOS/UEFI is needed to boot a real OS; use Bochs/SeaBIOS or write a minimal stub.
- The reset vector is at `0xffff0` (or `0xfffffff0` for 4 GiB ROM).
- Define a memory layout with low RAM, optional ISA/VGA holes, PCI, APIC, and BIOS.
- ACPI tells the OS about the platform: build RSDP, RSDT, MADT, FADT, and DSDT.
- The MADT lists vCPUs and the IO-APIC.
- The FADT points to the DSDT and declares power-management I/O ports.
- Use `iasl` to compile a DSDT, or use a precompiled binary.
