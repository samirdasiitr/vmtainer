/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

#ifndef BIOS_H
#define BIOS_H

#include <stdint.h>
#include "vmm.h"
#include "bios_offsets.h"

/*
 * Minimal boot / BIOS constants and structures (from vmtainer src/boot.hpp).
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

#define BIOS_BIN_PATH   "bios.bin"

/*
 * Install a minimal real-mode BIOS in the low 1 MiB, including the BIOS
 * blob, the IVT, the E820 map in the EBDA, and an optional VGA ROM stub.
 */
void bios_install(struct vmm *vmm);

#endif /* BIOS_H */
