#pragma once

#include <cstdint>

/*
 * Minimal boot protocol constants for loading a Linux bzImage.
 */

#define BZ_DEFAULT_SETUP_SECTS  4
#define BZ_KERNEL_START         0x100000UL
#define INITRD_START            0x1000000UL
#define BOOT_CMDLINE_OFFSET     0x20000UL
#define BOOT_LOADER_SELECTOR    0x1000
#define BOOT_LOADER_IP          0x0000
#define BOOT_LOADER_SP          0x8000

/* I/O ports */
#define SERIAL_PORT     0x3f8
#define SHUTDOWN_PORT   0x64

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << 26)
#endif

/*
 * BIOS / E820 definitions (from kvmtool, used to install a minimal real-mode BIOS).
 */
#define REAL_MODE_IVT_BEGIN     0x00000000
#define REAL_MODE_IVT_END       0x000003ff
#define BDA_START               0x00000400
#define BDA_END                 0x000004ff
#define EBDA_START              0x0009fc00
#define EBDA_END                0x0009ffff
#define E820_MAP_START          EBDA_START
#define VGA_RAM_BEGIN           0x000a0000
#define VGA_RAM_END             0x000bffff
#define VGA_ROM_BEGIN           0x000c0000
#define VGA_ROM_OEM_STRING      VGA_ROM_BEGIN
#define VGA_ROM_OEM_STRING_SIZE 16
#define VGA_ROM_MODES           (VGA_ROM_BEGIN + VGA_ROM_OEM_STRING_SIZE)
#define VGA_ROM_MODES_SIZE      32
#define VGA_ROM_END             0x000c7fff
#define MB_BIOS_BEGIN           0x000f0000
#define MB_BIOS_END             0x000fffff
#define MB_BIOS_SIZE            (MB_BIOS_END - MB_BIOS_BEGIN + 1)
#define REAL_INTR_VECTORS       256
#define REAL_INTR_SIZE          (REAL_INTR_VECTORS * 4)

#define E820_RAM        1
#define E820_RESERVED   2

struct e820entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} __attribute__((packed));

struct e820map {
    uint32_t nr_map;
    struct e820entry map[128];
};

struct real_intr_desc {
    uint16_t offset;
    uint16_t segment;
} __attribute__((packed));
