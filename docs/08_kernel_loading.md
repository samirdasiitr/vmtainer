<!--
Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.

PROPRIETARY AND CONFIDENTIAL.
Unauthorized copying, reproduction, distribution, or modification of this
file, via any medium, is strictly prohibited.
All rights reserved.
-->

# 08 — Loading and Booting a Linux Kernel

## 1. Booting a Linux Guest

A VMM can boot a Linux kernel in two main ways:

1. **Direct kernel boot** — the VMM parses the `bzImage` header, loads the setup and protected-mode kernel, and jumps to the 32-bit or 64-bit entry point. No full BIOS is required.
2. **Boot from a bootloader** — the VMM runs firmware (SeaBIOS, Bochs BIOS) or a bootloader (GRUB, syslinux) from a disk image, which in turn loads the kernel.

This chapter covers direct kernel boot because it is the most instructive for a from-scratch VMM. It also explains the `bzImage` structure, the boot protocol, the E820 map, and the transition to protected or long mode.

## 2. The Linux `bzImage` Format

A `bzImage` is an x86/x86_64 kernel that is self-extracting and self-relocating. It consists of two parts:

```
+-----------------------------------+
|  setup section  (512 bytes..)     |
|  - boot sector header (first 512) |
|  - setup header                     |
|  - initrd, cmdline info             |
+-----------------------------------+
|  compressed / protected kernel    |
|  - real kernel code + data          |
+-----------------------------------+
```

### Key offsets

- `0x000` — boot sector (first 512 bytes).
- `0x1f1` (byte) — setup sectors count `setup_sects`.
- `0x1f4` (2 bytes) — `syssize`, size of the 16-bit part (in 16-byte paras).
- `0x1f8` (2 bytes) — `root_flags`.
- `0x1fc` (2 bytes) — `header` magic `0xAA55`.
- `0x202` (2 bytes) — `jump` instruction or magic `0xHdrS`.
- `0x202`/`0x206` — the `HdrS` magic number `0x53726448`.
- `0x206` (2 bytes) — `version` of the boot protocol.
- `0x214` (4 bytes) — `code32_start`.
- `0x218` (4 bytes) — `ramdisk_image`.
- `0x21c` (4 bytes) — `ramdisk_size`.
- `0x228` (2 bytes) — `heap_end_ptr`.
- `0x22a` (1 byte) — `ext_loader_ver`.
- `0x22b` (1 byte) — `ext_loader_type`.
- `0x22c` (4 bytes) — `cmd_line_ptr`.
- `0x236` (4 bytes) — `initrd_addr_max`.
- `0x238` (4 bytes) — `kernel_alignment`.
- `0x23c` (1 byte) — `relocatable_kernel`.
- `0x250` (8 bytes) — `payload_offset`.
- `0x258` (8 bytes) — `payload_length`.
- `0x260` (8 bytes) — `setup_data`.
- `0x268` (8 bytes) — `pref_address`.
- `0x270` (4 bytes) — `init_size`.
- `0x274` (4 bytes) — `handover_offset`.

See `Documentation/x86/boot.rst` in the Linux kernel source for the canonical layout.

## 3. Setup Header Parsing

```c
#include <stdint.h>
#include <string.h>
#include <endian.h>

struct linux_setup_header {
    uint8_t  setup_sects;
    uint16_t root_flags;
    uint32_t syssize;
    uint16_t ram_size;
    uint16_t vid_mode;
    uint16_t root_dev;
    uint16_t boot_sector_patch;
    uint16_t header_version;
    uint32_t realmode_swtch;
    uint16_t start_sys;
    uint16_t kernel_version;
    uint8_t  type_of_loader;
    uint8_t  loadflags;
    uint16_t setup_move_size;
    uint32_t code32_start;
    uint32_t ramdisk_image;
    uint32_t ramdisk_size;
    uint32_t bootsect_kludge;
    uint16_t heap_end_ptr;
    uint8_t  ext_loader_ver;
    uint8_t  ext_loader_type;
    uint32_t cmd_line_ptr;
    uint32_t initrd_addr_max;
    uint32_t kernel_alignment;
    uint8_t  relocatable_kernel;
    uint8_t  min_alignment;
    uint16_t xloadflags;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint64_t setup_data;
    uint64_t pref_address;
    uint32_t init_size;
    uint32_t handover_offset;
} __attribute__((packed));

/* Read the 512-byte boot sector header into this subset. */
#define LINUX_BOOT_HEADER_MAGIC 0x1f1

void *load_bzimage(void *guest_mem, size_t guest_size,
                   const char *path, uint32_t *entry)
{
    /* Read the kernel image. */
    uint8_t *image = read_file(path);
    size_t image_size = file_size(path);

    /* The first 512 bytes. */
    uint8_t *header = image;
    if (header[0x1fe] != 0x55 || header[0x1ff] != 0xaa)
        errx(1, "Not a bootable bzImage");

    if (memcmp(header + 0x202, "HdrS", 4) != 0)
        errx(1, "Missing HdrS magic");

    uint8_t setup_sects = header[0x1f1];
    if (setup_sects == 0) setup_sects = 4;
    size_t setup_size = (setup_sects + 1) * 512;

    uint32_t code32_start = *(uint32_t *)(header + 0x214);
    if (code32_start == 0)
        code32_start = 0x100000;  /* default: 1 MiB */

    /* Copy setup section to 0x90200 or 0x10000? Modern kernels use 0x100000. */
    uint32_t setup_base = 0x100000 - setup_size;  /* old style */
    (void)setup_base;

    return image;
}
```

For most `bzImage`s, the setup section is loaded at `0x100000` (1 MiB). The `code32_start` field tells the protected-mode entry point. Modern x86_64 `bzImage`s expect to be loaded at `0x100000` and entered at `0x100000 + 0x200` (or the 64-bit entry at `0x100000 + setup_size`).

## 4. The Boot Protocol

The VMM is the **boot loader**. It must fill in the setup header and then jump to the correct entry point. The kernel expects:

- `setup` section at `0x100000`.
- Protected-mode kernel immediately after the setup.
- E820 memory map at a known physical address.
- Command line string at `cmd_line_ptr`.
- Initrd at `ramdisk_image` with size `ramdisk_size`.
- `loadflags` bits set correctly:
  - bit 0 (`LOADED_HIGH`): 1 for bzImage.
  - bit 5 (`CAN_USE_HEAP`): use the heap.
  - bit 7 (`KEEP_SEGMENTS`): preserve segments.

### Minimal header setup

```c
void prepare_setup_header(uint8_t *setup, const char *cmdline)
{
    /* Set loadflags. */
    uint8_t *loadflags = setup + 0x211;
    *loadflags |= 0x01;   /* LOADED_HIGH */
    *loadflags |= 0x20;   /* CAN_USE_HEAP */

    /* Heap end. */
    *(uint16_t *)(setup + 0x228) = 0x9000;

    /* Type of loader: 0xff = unknown, but custom. */
    *(uint8_t *)(setup + 0x210) = 0xff;

    /* Command line. */
    uint32_t cmdline_gpa = 0x20000;
    *(uint32_t *)(setup + 0x228 + 4) = cmdline_gpa;  /* cmd_line_ptr */

    /* Initrd: will be filled later. */
    /* ramdisk_image = 0x400000; */
    /* ramdisk_size  = initrd_len; */

    /* Video mode: 0xffff means normal. */
    *(uint16_t *)(setup + 0x206 + 0x?)
}
```

A more robust approach is to load the whole `bzImage` at `0x100000`, then fill in the fields:

```c
int fd = open(path, O_RDONLY);
struct stat st; fstat(fd, &st);
void *image = malloc(st.st_size);
read(fd, image, st.st_size);
close(fd);

uint8_t setup_sects = ((uint8_t *)image)[0x1f1];
if (setup_sects == 0) setup_sects = 4;
size_t setup_len = (setup_sects + 1) * 512;

memcpy((uint8_t *)guest_mem + 0x100000, image, st.st_size);

uint8_t *setup = (uint8_t *)guest_mem + 0x100000;

/* Poke fields directly in guest memory. */
setup[0x210] = 0xff;   /* type_of_loader */
setup[0x211] |= 0x01;  /* LOADED_HIGH */
setup[0x211] |= 0x20;  /* CAN_USE_HEAP */

/* Command line at 0x20000. */
char *cmdline_dst = (char *)guest_mem + 0x20000;
strncpy(cmdline_dst, cmdline, 0xff0);
*(uint32_t *)(setup + 0x228) = 0x20000;
```

## 5. E820 Memory Map

The **E820 map** tells the OS which physical address ranges are RAM, reserved, or ACPI data. It is passed to the kernel via `struct boot_params` or the `setup` header pointer. For a simple VMM, you can use the old `SMAP` method: write a table of `e820entry` and point `setup` to it.

```c
struct e820entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} __attribute__((packed));

#define E820_RAM      1
#define E820_RESERVED 2
#define E820_ACPI     3
#define E820_NVS      4
#define E820_UNUSABLE 5

void build_e820(void *guest_mem, size_t ram_size)
{
    struct e820entry *e = (struct e820entry *)((uint8_t *)guest_mem + 0x2d000);

    e[0].addr = 0;
    e[0].size = 0x9fc00;        /* low 640 KiB minus EBDA */
    e[0].type = E820_RAM;

    e[1].addr = 0x9fc00;
    e[1].size = 0x400;          /* EBDA */
    e[1].type = E820_RESERVED;

    e[2].addr = 0xf0000;
    e[2].size = 0x10000;        /* ISA/BIOS region */
    e[2].type = E820_RESERVED;

    e[3].addr = 0x100000;
    e[3].size = ram_size - 0x100000;
    e[3].type = E820_RAM;

    e[4].addr = 0xfee00000;
    e[4].size = 0x1000;         /* LAPIC */
    e[4].type = E820_RESERVED;

    e[5].addr = 0xfec00000;
    e[5].size = 0x1000;         /* IO-APIC */
    e[5].type = E820_RESERVED;

    uint8_t *setup = (uint8_t *)guest_mem + 0x100000;
    /* Pointer to e820 table: kernel uses offsets in struct boot_params. */
    /* For the old protocol: fill e820_map in the setup. */
}
```

The precise layout depends on the boot protocol version. Modern kernels use `struct boot_params` at `0x10000` or `0x100000` with `e820_map` and `e820_entries`.

## 6. 32-bit Protected Mode Entry

For 32-bit `bzImage`:

1. Load the setup at `0x100000`.
2. Set `cs = 0x10` (flat 32-bit code), `ds/es/ss = 0x18` (flat 32-bit data).
3. Disable paging and protected mode in `cr0` first, then set `cr0.PE = 1`, `cr0.PG = 0`.
4. Set `eip = 0x100000 + 0x200` (the `startup_32` entry).
5. Jump with `KVM_SET_REGS` and `KVM_SET_SREGS`.

```c
struct kvm_sregs sregs = {0};
struct kvm_regs regs = {0};

sregs.cs.selector = 0x10;
sregs.cs.base = 0;
sregs.cs.limit = 0xffffffff;
sregs.cs.g = 1;   /* 4 KiB granularity */
sregs.cs.db = 1;  /* 32-bit */
sregs.cs.s = 1;   /* user/code or data */
sregs.cs.type = 0xb;  /* execute/read, non-conforming, accessed */
sregs.cs.dpl = 0;
sregs.cs.present = 1;

sregs.ds = sregs.es = sregs.ss = sregs.cs;
sregs.ds.type = 0x3;  /* read/write, accessed */

sregs.gdt.base = 0;
sregs.gdt.limit = 0xffff;
sregs.idt.base = 0;
sregs.idt.limit = 0xffff;

sregs.cr0 = 0x00000001 | 0x00000010;  /* PE=1, ET=1, NE=1, MP=1 possibly */

regs.rip = 0x100200;  /* 0x100000 + 0x200 */
regs.rsi = 0x100000;  /* some kernels expect boot_params ptr in esi */
regs.rsp = 0x8f000;
regs.rflags = 0x2;
```

## 7. 64-bit Long Mode Entry

A 64-bit `bzImage` starts in 32-bit protected mode and then enables long mode itself. The VMM must provide:

- 32-bit flat segments (as above).
- `cr0` with `PE=1`, `PG=0`.
- `cr4.PAE=1`.
- `efer.LME=1`.
- A page table at `cr3`.

When the kernel's 32-bit `startup_32` enables paging, the CPU transitions to long mode.

```c
uint64_t pml4[512] __attribute__((aligned(4096))) = {0};
uint64_t pdpt[512] __attribute__((aligned(4096))) = {0};

pml4[0] = (uint64_t)(uintptr_t)pdpt | 0x03;  /* present, write */
pdpt[0] = 0x03 | 0x80;  /* 1 GiB huge page at 0, present, write, PS=1 */

/* Or map a real page table into guest memory and get its GPA. */

sregs.cr3 = 0x200000;  /* pml4 physical */
sregs.cr4 = 0x00000020;  /* PAE=1 */
sregs.efer = 0x00000100;  /* LME=1 */
sregs.cr0 = 0x00000001 | 0x00000020 | 0x80000000;  /* PE, MP, PG */
```

With the in-kernel `KVM_CREATE_IRQCHIP`, the kernel can then initialize the local APIC and continue into 64-bit startup.

## 8. Loading the Initrd

The initial ramdisk is loaded at a high physical address where the kernel expects it. Common choices are `0x400000` (4 MiB) or the address specified by `initrd_addr_max` (below the physical limit). The setup header `ramdisk_image` and `ramdisk_size` point to it.

```c
void load_initrd(void *guest_mem, size_t guest_size,
                 const char *path, uint32_t *addr, uint32_t *size)
{
    uint8_t *img = read_file(path);
    *size = file_size(path);

    *addr = 0x400000;  /* 4 MiB; must be below initrd_addr_max */
    if (*addr + *size > guest_size)
        errx(1, "initrd does not fit");

    memcpy((uint8_t *)guest_mem + *addr, img, *size);
    free(img);
}
```

Then set:

```c
*(uint32_t *)(setup + 0x218) = *addr;   /* ramdisk_image */
*(uint32_t *)(setup + 0x21c) = *size;   /* ramdisk_size */
```

## 9. A Complete Direct-Boot Example

```c
int boot_kernel(int vm_fd, int vcpu_fd, void *guest_mem, size_t guest_size,
                const char *bzimage, const char *initrd,
                const char *cmdline)
{
    uint32_t initrd_addr, initrd_size;

    /* 1. Load kernel at 1 MiB. */
    load_file(guest_mem, 0x100000, bzimage);

    /* 2. Load initrd and set setup fields. */
    if (initrd)
        load_initrd(guest_mem, guest_size, initrd, &initrd_addr, &initrd_size);

    uint8_t *setup = (uint8_t *)guest_mem + 0x100000;
    setup[0x210] = 0xff;            /* type_of_loader */
    setup[0x211] |= 0x01 | 0x20;    /* LOADED_HIGH | CAN_USE_HEAP */

    if (cmdline) {
        memset(guest_mem + 0x20000, 0, 0x1000);
        strncpy(guest_mem + 0x20000, cmdline, 0xff0);
        *(uint32_t *)(setup + 0x228) = 0x20000;  /* cmd_line_ptr */
    }

    if (initrd) {
        *(uint32_t *)(setup + 0x218) = initrd_addr;
        *(uint32_t *)(setup + 0x21c) = initrd_size;
    }

    /* 3. Build E820. */
    build_e820(guest_mem, guest_size);

    /* 4. Set vCPU state for 32-bit protected mode entry. */
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    ioctl(vcpu_fd, KVM_GET_SREGS, &sregs);
    ioctl(vcpu_fd, KVM_GET_REGS, &regs);

    sregs.cs.selector = 0x10;
    sregs.cs.base = 0;
    sregs.cs.limit = 0xffffffff;
    sregs.cs.g = 1;
    sregs.cs.db = 1;
    sregs.cs.type = 0xb;
    sregs.cs.present = 1;

    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = sregs.cs;
    sregs.ds.type = 0x3;
    sregs.ss.type = 0x3;

    sregs.gdt.base = 0;
    sregs.gdt.limit = 0xffff;

    sregs.efer = 0x100;  /* LME=1 for 64-bit bzImage; cleared for 32-bit */
    sregs.cr4 = 0x20;    /* PAE */
    sregs.cr3 = 0x200000;
    sregs.cr0 = 0x80000001 | 0x20;  /* PG, PE, MP */
    sregs.apic_base = 0xfee00000 | 0x800;

    ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);

    regs.rip = 0x100200;  /* 0x100000 + 0x200 */
    regs.rsi = 0x100000;  /* boot_params */
    regs.rsp = 0x8f000;
    regs.rflags = 0x2;

    ioctl(vcpu_fd, KVM_SET_REGS, &regs);

    /* 5. Run the vCPU. */
    for (;;) {
        if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR) continue;
            err(1, "KVM_RUN");
        }
        dispatch_exit(vcpu_fd, run);
    }
}
```

## 10. Command Line

A typical command line for the example above:

```
console=ttyS0,115200n8 root=/dev/ram0 rw init=/bin/sh
```

This tells Linux to use the first serial port for early output, to mount the initrd, and to run `/bin/sh`. Without a command line, the kernel will panic if it cannot find a root device.

## 11. Summary

- `bzImage` = setup section + compressed kernel.
- The setup header (`0x100000` + `0x1f1`/`0x211` etc.) controls boot.
- Load setup + kernel at `0x100000`.
- Load initrd at a high address and set `ramdisk_image`/`ramdisk_size`.
- Set `cmd_line_ptr` and write the string to guest memory.
- Build an E820 memory map.
- Set 32-bit or 64-bit initial state with `KVM_SET_SREGS`/`KVM_SET_REGS`.
- 64-bit `bzImage` starts in 32-bit mode and enables long mode by setting `CR0.PG` with `EFER.LME` and `CR4.PAE`.
- The VMM must provide a page table and GDT/IDT for the initial jump.
