/*
 * bios.c
 *
 * Install the real-mode BIOS used by the example VMM, mirroring what
 * vmtainer's main VMM (src/vmm.cpp) does for booting a bzImage.
 *
 * This loads the raw 16-bit BIOS blob into the 0xF0000-0xFFFFF window,
 * sets up the BDA/EBDA, builds an E820 map in the EBDA, installs a real-mode
 * IVT at address 0, and places a tiny stub for the VGA option ROM.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vmm.h"
#include "bios.h"

/*
 * Read an entire file into a heap buffer.  Returns 0 on success and sets
 * *out / *out_size.  On error returns -1 and logs.
 */
static int read_bios_file(const char *path, uint8_t **out, size_t *out_size)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG("cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        LOG("fstat %s failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    *out_size = (size_t)st.st_size;
    *out = malloc(*out_size);
    if (!*out) {
        LOG("malloc for %s failed", path);
        close(fd);
        return -1;
    }

    size_t off = 0;
    while (off < *out_size) {
        ssize_t n = read(fd, *out + off, *out_size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG("read %s failed: %s", path, strerror(errno));
            free(*out);
            close(fd);
            return -1;
        }
        if (n == 0) {
            *out_size = off;
            break;
        }
        off += (size_t)n;
    }

    close(fd);
    LOG("read %s (%zu bytes)", path, *out_size);
    return 0;
}

/*
 * Build the E820 memory map for the BIOS int15h/ah=e820 handler.  This is
 * the same map installed by the main vmtainer VMM.
 */
static void setup_e820(struct vmm *vmm)
{
    struct e820map *e820 = (struct e820map *)(vmm->mem + E820_MAP_START);
    uint32_t i = 0;

    e820->map[i].addr  = REAL_MODE_IVT_BEGIN;
    e820->map[i].size  = EBDA_START - REAL_MODE_IVT_BEGIN;
    e820->map[i].type  = E820_RAM;
    i++;

    e820->map[i].addr  = EBDA_START;
    e820->map[i].size  = VGA_RAM_BEGIN - EBDA_START;
    e820->map[i].type  = E820_RESERVED;
    i++;

    e820->map[i].addr  = VGA_RAM_BEGIN;
    e820->map[i].size  = MB_BIOS_BEGIN - VGA_RAM_BEGIN;
    e820->map[i].type  = E820_RESERVED;
    i++;

    e820->map[i].addr  = MB_BIOS_BEGIN;
    e820->map[i].size  = MB_BIOS_SIZE;
    e820->map[i].type  = E820_RESERVED;
    i++;

    e820->map[i].addr  = 0x100000ULL;
    e820->map[i].size  = vmm->mem_size - 0x100000ULL;
    e820->map[i].type  = E820_RAM;
    i++;

    e820->nr_map = i;
    LOG("installed E820 map with %u entries at 0x%05x",
        e820->nr_map, E820_MAP_START);
}

/*
 * Minimal VGA option ROM header as expected by int10h / some boot probes.
 */
static void setup_vga_rom(struct vmm *vmm)
{
    char *oem = (char *)(vmm->mem + VGA_ROM_OEM_STRING);
    uint16_t *modes = (uint16_t *)(vmm->mem + VGA_ROM_MODES);

    memset(vmm->mem + VGA_ROM_BEGIN, 0,
           VGA_ROM_END - VGA_ROM_BEGIN + 1);
    strncpy(oem, "KVM VESA", VGA_ROM_OEM_STRING_SIZE);
    modes[0] = 0x0112;
    modes[1] = 0xffff;
    LOG("installed minimal VGA ROM at 0x%05x", VGA_ROM_BEGIN);
}

/*
 * Build the real-mode IVT.  Every vector initially points to a harmless
 * iret-like stub (bios_intfake).  The two vectors the bzImage real-mode
 * setup actually calls, 0x10 (video) and 0x15 (BIOS services / E820),
 * are redirected to the real handlers in the BIOS blob.
 */
static void install_ivt(struct vmm *vmm)
{
    struct real_intr_desc ivt[REAL_INTR_VECTORS];
    struct real_intr_desc fake = {
        .offset  = (uint16_t)BIOS_OFFSET__bios_intfake,
        .segment = (uint16_t)(MB_BIOS_BEGIN >> 4),
    };

    for (int i = 0; i < REAL_INTR_VECTORS; i++)
        ivt[i] = fake;

    ivt[0x10].offset  = (uint16_t)BIOS_OFFSET__bios_int10;
    ivt[0x10].segment = (uint16_t)(MB_BIOS_BEGIN >> 4);
    ivt[0x15].offset  = (uint16_t)BIOS_OFFSET__bios_int15;
    ivt[0x15].segment = (uint16_t)(MB_BIOS_BEGIN >> 4);

    memcpy(vmm->mem + REAL_MODE_IVT_BEGIN, ivt, sizeof(ivt));
    LOG("installed real-mode IVT (0x%04x bytes)", REAL_INTR_SIZE);
}

/*
 * MP (multi-processor) floating pointer and configuration table.
 * The floating pointer lives in the last 64 KiB of BIOS, and the
 * configuration table follows it.  This is the legacy way to tell Linux
 * how many CPUs are present and where the local APIC lives.
 */

#define MP_TABLE_BASE   (MB_BIOS_BEGIN + 0xd000)
#define MP_FPTR_ADDR    MP_TABLE_BASE
#define MP_TABLE_ADDR   (MP_TABLE_BASE + 16)

struct mp_fptr {
    uint32_t signature;          /* "_MP_" */
    uint32_t table_addr;         /* physical address of MP config table */
    uint8_t  length;             /* length of floating pointer in 16-byte units */
    uint8_t  spec_rev;
    uint8_t  checksum;
    uint8_t  feature1;
    uint8_t  feature2;
    uint8_t  feature3;
    uint8_t  feature4;
    uint8_t  feature5;
} __attribute__((packed));

struct mp_table_head {
    char     signature[4];       /* "PCMP" */
    uint16_t base_table_length;
    uint8_t  spec_rev;
    uint8_t  checksum;
    char     oem_id[8];
    char     product_id[12];
    uint32_t oem_table_ptr;
    uint16_t oem_table_size;
    uint16_t entry_count;
    uint32_t local_apic_addr;
    uint16_t ext_table_length;
    uint8_t  ext_table_checksum;
    uint8_t  reserved;
} __attribute__((packed));

struct mp_cpu {
    uint8_t  entry_type;         /* 0 = CPU */
    uint8_t  local_apic_id;
    uint8_t  local_apic_version;
    uint8_t  cpu_flags;
    uint32_t cpu_signature;
    uint32_t feature_flags;
    uint32_t reserved[2];
} __attribute__((packed));

struct mp_bus {
    uint8_t  entry_type;         /* 1 = bus */
    uint8_t  bus_id;
    char     bus_type[6];
} __attribute__((packed));

struct mp_io_apic {
    uint8_t  entry_type;         /* 2 = I/O APIC */
    uint8_t  apic_id;
    uint8_t  apic_version;
    uint8_t  apic_flags;
    uint32_t apic_addr;
} __attribute__((packed));

struct mp_irq {
    uint8_t  entry_type;         /* 3 = I/O interrupt */
    uint8_t  int_type;
    uint16_t irq_flags;
    uint8_t  src_bus;
    uint8_t  src_irq;
    uint8_t  dst_apic;
    uint8_t  dst_irq;
} __attribute__((packed));

static uint8_t mp_checksum(uint8_t *data, size_t len, size_t checksum_off)
{
    data[checksum_off] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += data[i];
    return (uint8_t)(-sum);
}

static void setup_mp_table(struct vmm *vmm)
{
    uint8_t *mem = vmm->mem;
    struct mp_fptr *fptr = (struct mp_fptr *)(mem + MP_FPTR_ADDR);
    struct mp_table_head *head = (struct mp_table_head *)(mem + MP_TABLE_ADDR);
    size_t nr_cpus = (size_t)vmm->nr_vcpus;

    memset(fptr, 0, sizeof(*fptr));
    memcpy(&fptr->signature, "_MP_", 4);   /* little-endian word 0x5f504d5f */
    fptr->table_addr = (uint32_t)MP_TABLE_ADDR;
    fptr->length = 1;                       /* 16 bytes */
    fptr->spec_rev = 4;
    fptr->checksum = mp_checksum((uint8_t *)fptr, sizeof(*fptr),
                                 offsetof(struct mp_fptr, checksum));

    memset(head, 0, sizeof(*head));
    memcpy(head->signature, "PCMP", 4);
    head->spec_rev = 4;
    memcpy(head->oem_id, "EXVMM   ", 8);
    memcpy(head->product_id, "EXAMPLE     ", 12);
    head->local_apic_addr = 0xfee00000;
    head->entry_count = (uint16_t)(nr_cpus + 1 + 1 + 16);

    struct mp_cpu *cpu = (struct mp_cpu *)(head + 1);
    for (size_t i = 0; i < nr_cpus; i++) {
        memset(cpu, 0, sizeof(*cpu));
        cpu->entry_type = 0;
        cpu->local_apic_id = (uint8_t)i;
        cpu->local_apic_version = 0x14;
        cpu->cpu_flags = (i == 0) ? 0x03 : 0x01; /* BSP enabled + online */
        cpu->cpu_signature = 0x0000;
        cpu->feature_flags = 0x00000001;         /* FPU */
        cpu++;
    }

    struct mp_bus *bus = (struct mp_bus *)cpu;
    memset(bus, 0, sizeof(*bus));
    bus->entry_type = 1;
    bus->bus_id = 0;
    memcpy(bus->bus_type, "ISA   ", 6);
    bus++;

    struct mp_io_apic *ioa = (struct mp_io_apic *)bus;
    memset(ioa, 0, sizeof(*ioa));
    ioa->entry_type = 2;
    ioa->apic_id = (uint8_t)(nr_cpus + 1);
    ioa->apic_version = 0x11;
    ioa->apic_flags = 0x01;        /* enabled */
    ioa->apic_addr = 0xfec00000;
    ioa++;

    struct mp_irq *irq = (struct mp_irq *)ioa;
    for (int i = 0; i < 16; i++) {
        memset(irq, 0, sizeof(*irq));
        irq->entry_type = 3;
        irq->int_type = 0;         /* INT */
        irq->irq_flags = 0x000f;   /* conform polarity and trigger */
        irq->src_bus = 0;
        irq->src_irq = (uint8_t)i;
        irq->dst_apic = (uint8_t)(nr_cpus + 1);
        irq->dst_irq = (uint8_t)i;
        irq++;
    }

    head->base_table_length =
        sizeof(*head) + nr_cpus * sizeof(*cpu) +
        sizeof(*bus) + sizeof(*ioa) + 16 * sizeof(*irq);
    head->checksum = mp_checksum((uint8_t *)head, head->base_table_length,
                                 offsetof(struct mp_table_head, checksum));

    LOG("installed MP table at 0x%05x (%zu CPUs)", MP_FPTR_ADDR, nr_cpus);
}

/*
 * Install the real-mode BIOS image, IVT, E820 map, and related low-memory
 * structures.  This must run after guest memory is mapped but before the
 * bzImage real-mode setup code is copied into the low 1 MiB.
 */
void bios_install(struct vmm *vmm)
{
    uint8_t *bios = NULL;
    size_t   bios_size = 0;

    /*
     * Clear the BIOS and related low-memory regions.  The rest of guest
     * memory is already zeroed by the anonymous mmap in vmm_map_memory().
     */
    memset(vmm->mem + BDA_START,   0, BDA_END - BDA_START + 1);
    memset(vmm->mem + EBDA_START,  0, EBDA_END - EBDA_START + 1);
    memset(vmm->mem + MB_BIOS_BEGIN, 0, MB_BIOS_SIZE);

    /*
     * Load the real-mode BIOS blob into the 0xF0000-0xFFFFF window.
     * The blob is installed before any IVT vectors are set so the
     * handlers live at the offsets defined in bios_offsets.h.
     */
    if (read_bios_file(BIOS_BIN_PATH, &bios, &bios_size) < 0) {
        LOG("WARNING: could not load %s; the VMM may not boot", BIOS_BIN_PATH);
    } else {
        if (bios_size > (size_t)MB_BIOS_SIZE)
            bios_size = MB_BIOS_SIZE;
        memcpy(vmm->mem + MB_BIOS_BEGIN, bios, bios_size);
        free(bios);
        LOG("installed BIOS blob at 0x%05x (0x%zx bytes)",
            MB_BIOS_BEGIN, bios_size);
    }

    setup_e820(vmm);
    setup_vga_rom(vmm);
    install_ivt(vmm);
    setup_mp_table(vmm);

    /*
     * Real-mode AP reset vector at 0xF000:0xFFF0 (linear 0xFFFF0).
     * cli ; hlt keeps the AP stopped until the OS re-initializes it.
     */
    uint8_t *rv = vmm->mem + MB_BIOS_END - 15;
    rv[0] = 0xfa;       /* cli */
    rv[1] = 0xf4;       /* hlt */
    memset(rv + 2, 0x90, 13);
}
