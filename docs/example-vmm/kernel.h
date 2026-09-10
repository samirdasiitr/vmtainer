#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include "vmm.h"

/*
 * The two E820 types we actually use in the minimal memory map.
 */
#define E820_RAM      1
#define E820_RESERVED 2

/*
 * The first 0x1F1 bytes of the bzImage are the boot sector.  The
 * setup_header starts at offset 0x1F1 and describes the kernel image.
 * This packed structure matches the Linux x86 boot protocol well enough
 * for a tiny bzImage + initrd.
 */
struct setup_header {
    uint8_t  setup_sects;          /* 0x1f1 */
    uint16_t root_flags;           /* 0x1f2 */
    uint32_t syssize;              /* 0x1f4 */
    uint16_t ram_size;             /* 0x1f8 */
    uint16_t vid_mode;             /* 0x1fa */
    uint16_t root_dev;             /* 0x1fc */
    uint16_t boot_flag;            /* 0x1fe */
    uint16_t jump;                 /* 0x200 */
    uint8_t  magic[4];             /* 0x202, should be "HdrS" */
    uint16_t version;              /* 0x206 */
    uint8_t  realmode_swtch[4];    /* 0x208 */
    uint16_t start_sys;            /* 0x20c */
    uint16_t kernel_version;       /* 0x20e */
    uint8_t  type_of_loader;       /* 0x210 */
    uint8_t  loadflags;            /* 0x211 */
    uint16_t setup_move_size;      /* 0x212 */
    uint32_t code32_start;         /* 0x214 */
    uint32_t ramdisk_image;        /* 0x218 */
    uint32_t ramdisk_size;         /* 0x21c */
    uint32_t bootsect_kludge;      /* 0x220 */
    uint16_t heap_end_ptr;         /* 0x224 */
    uint8_t  ext_loader_ver;       /* 0x226 */
    uint8_t  ext_loader_type;      /* 0x227 */
    uint32_t cmd_line_ptr;         /* 0x228 */
    uint32_t initrd_addr_max;      /* 0x22c */
    uint32_t kernel_alignment;     /* 0x230 */
    uint8_t  relocatable_kernel;   /* 0x234 */
    uint8_t  min_alignment;        /* 0x235 */
    uint16_t xloadflags;           /* 0x236 */
    uint32_t cmdline_size;         /* 0x238 */
    uint32_t hardware_subarch;     /* 0x23c */
    uint64_t hardware_subarch_data;/* 0x240 */
    uint32_t payload_offset;       /* 0x248 */
    uint32_t payload_length;       /* 0x24c */
    uint64_t setup_data;           /* 0x250 */
    uint64_t pref_address;         /* 0x258 */
    uint32_t init_size;            /* 0x260 */
    uint32_t handover_offset;      /* 0x264 */
} __attribute__((packed));

/*
 * A single E820 memory range.  The Linux kernel uses this to build its
 * physical memory map.
 */
struct e820_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} __attribute__((packed));

/*
 * Very small subset of the Linux zero-page (struct boot_params).
 * We only need the E820 entry count and the map.  The full boot protocol
 * zero-page is 4096 bytes at 0x10000.
 */
struct e820_layout {
    uint8_t        _pad0[0x1e8];              /* 0x000 - 0x1e7 */
    uint8_t        e820_entries;              /* 0x1e8 */
    uint8_t        _pad1[0x2d0 - 0x1e8 - 1];  /* 0x1e9 - 0x2cf */
    struct e820_entry e820_map[32];           /* 0x2d0 */
} __attribute__((packed));

/*
 * Load a bzImage and optional initrd into guest RAM, fill the setup_header
 * and the E820 map, and place the kernel command line.
 */
int kernel_load(struct vmm *vmm, const char *kernel_path,
                const char *initrd_path, const char *cmdline);

#endif /* KERNEL_H */
