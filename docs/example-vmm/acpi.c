/*
 * acpi.c
 *
 * Constructs minimal ACPI tables in guest memory.  The goal is to show how
 * the RSDP, RSDT, XSDT, MADT, FADT, and DSDT relate to one another and how
 * to compute the ACPI checksums.
 *
 * These tables are intentionally tiny; a real VMM would need a fully
 * populated FADT, a real DSDT with AML, an IO-APIC, etc.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "acpi.h"

/*
 * ACPI tables live in the reserved 0xE0000-0xFFFFF region so the Linux
 * early boot scanner can find the RSDP.
 */
#define ACPI_BASE 0xE0000ULL

/* Generic ACPI table header (36 bytes). */
struct acpi_table_head {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Root System Description Pointer (ACPI 2.0). */
struct rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* Multiple APIC Description Table. */
struct madt {
    struct acpi_table_head head;
    uint32_t               local_apic_address;
    uint32_t               flags;
    uint8_t                ics[];    /* variable-length APIC structures */
} __attribute__((packed));

/* Processor-local APIC entry inside the MADT. */
struct madt_lapic {
    uint8_t  type;       /* 0 = processor local APIC */
    uint8_t  length;     /* 8 */
    uint8_t  processor_id;
    uint8_t  local_apic_id;
    uint32_t flags;      /* 1 = enabled */
} __attribute__((packed));

/*
 * Minimal FADT.  Linux only needs the DSDT pointer for early boot, so the
 * table is small.  The 32-bit DSDT address is at the standard offset right
 * after firmware_ctrl.
 */
struct fadt {
    struct acpi_table_head head;
    uint32_t               firmware_ctrl;
    uint32_t               dsdt;
} __attribute__((packed));

/*
 * Compute an ACPI checksum: set the checksum field to 0, sum every byte,
 * then store the two's complement low byte.  After this the full sum is 0.
 */
static void set_checksum(uint8_t *data, size_t len, size_t checksum_offset)
{
    data[checksum_offset] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += data[i];
    data[checksum_offset] = (uint8_t)(-sum);
}

/*
 * Helper to fill the common 36-byte table header.
 */
static void fill_table_head(struct acpi_table_head *h, const char *sig,
                            uint8_t rev, uint32_t len)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->signature, sig, 4);
    h->length         = len;
    h->revision       = rev;
    h->checksum       = 0;
    memcpy(h->oem_id, "EXVMM", 5);          /* padded with NULs to 6 bytes */
    memcpy(h->oem_table_id, "EXVMM  ", 8);
    h->oem_revision   = 1;
    h->creator_id     = 0x564D4D45;          /* "EMMV"? little-endian "EXVM" */
    h->creator_revision = 1;
}

/*
 * A little allocator that builds tables one after another inside the
 * 0xE0000-0xEFFFF region.  It keeps the RSDP on a 16-byte boundary as
 * required by the ACPI specification.
 */
struct acpi_ctx {
    uint8_t *base;
    size_t   off;
    int      nr;
    uint64_t tables[8];
};

static size_t acpi_alloc(struct acpi_ctx *ctx, size_t size, size_t align)
{
    /* Align the current offset up to `align` bytes. */
    if (align && (ctx->off % align))
        ctx->off += align - (ctx->off % align);
    size_t p = ctx->off;
    ctx->off += size;
    return p;
}

int acpi_install(struct vmm *vmm)
{
    if (vmm->mem_size < ACPI_BASE + 0x10000) {
        LOG("guest memory too small for ACPI tables");
        return -1;
    }

    struct acpi_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.base = vmm->mem + ACPI_BASE;

    /*
     * 1) RSDP is always first and 16-byte aligned.  It links to the RSDT
     *    and (for ACPI 2.0) the XSDT.  The remaining tables follow.
     */
    size_t rsdp_off = acpi_alloc(&ctx, sizeof(struct rsdp), 16);
    struct rsdp *rsdp = (struct rsdp *)(ctx.base + rsdp_off);
    memset(rsdp, 0, sizeof(*rsdp));
    memcpy(rsdp->signature, "RSD PTR ", 8);
    memcpy(rsdp->oem_id, "EXVMM ", 6);
    rsdp->revision = 2;

    /*
     * 2) RSDT and XSDT.  They both point to the MADT and FADT.  The DSDT
     *    is linked from the FADT, not listed here.
     */
    const int nr_tables = 2;   /* MADT, FADT */
    size_t rsdt_size = sizeof(struct acpi_table_head) + nr_tables * 4;
    size_t rsdt_off  = acpi_alloc(&ctx, rsdt_size, 16);
    struct acpi_table_head *rsdt = (struct acpi_table_head *)(ctx.base + rsdt_off);
    fill_table_head(rsdt, "RSDT", 1, (uint32_t)rsdt_size);
    uint32_t *rsdt_entries = (uint32_t *)(rsdt + 1);

    size_t xsdt_size = sizeof(struct acpi_table_head) + nr_tables * 8;
    size_t xsdt_off  = acpi_alloc(&ctx, xsdt_size, 16);
    struct acpi_table_head *xsdt = (struct acpi_table_head *)(ctx.base + xsdt_off);
    fill_table_head(xsdt, "XSDT", 1, (uint32_t)xsdt_size);
    uint64_t *xsdt_entries = (uint64_t *)(xsdt + 1);

    /*
     * 3) MADT with one processor-local-APIC entry per vCPU.
     *    The BSP is processor 0 / APIC 0, the APs follow.
     */
    size_t madt_ics_len = vmm->nr_vcpus * sizeof(struct madt_lapic);
    size_t madt_size    = sizeof(struct madt) + madt_ics_len;
    size_t madt_off     = acpi_alloc(&ctx, madt_size, 16);
    struct madt *madt   = (struct madt *)(ctx.base + madt_off);
    fill_table_head(&madt->head, "APIC", 1, (uint32_t)madt_size);
    madt->local_apic_address = 0xFEE00000;
    madt->flags              = 0;

    for (int i = 0; i < vmm->nr_vcpus; i++) {
        struct madt_lapic *lapic;
        /* The ics array is unbounded, so use pointer arithmetic. */
        lapic = (struct madt_lapic *)(madt->ics + i * sizeof(struct madt_lapic));
        lapic->type        = 0;
        lapic->length      = sizeof(struct madt_lapic);
        lapic->processor_id = (uint8_t)i;
        lapic->local_apic_id = (uint8_t)i;
        lapic->flags       = 1;          /* enabled */
    }

    /*
     * 4) DSDT stub.  A real DSDT would contain AML.  For this skeleton we
     *    only need a valid table header so that the FADT has a target.
     */
    size_t dsdt_size = sizeof(struct acpi_table_head);
    size_t dsdt_off  = acpi_alloc(&ctx, dsdt_size, 16);
    struct acpi_table_head *dsdt = (struct acpi_table_head *)(ctx.base + dsdt_off);
    fill_table_head(dsdt, "DSDT", 2, (uint32_t)dsdt_size);

    /*
     * 5) FADT.  The important field is the DSDT pointer (32-bit).  For a
     *    more complete table one would also set the 64-bit x_dsdt.
     */
    size_t fadt_size = sizeof(struct fadt);
    size_t fadt_off  = acpi_alloc(&ctx, fadt_size, 16);
    struct fadt *fadt = (struct fadt *)(ctx.base + fadt_off);
    memset(fadt, 0, sizeof(*fadt));
    fill_table_head(&fadt->head, "FACP", 2, (uint32_t)fadt_size);
    fadt->firmware_ctrl = 0;
    fadt->dsdt          = (uint32_t)(ACPI_BASE + dsdt_off);

    /*
     * Fill RSDT / XSDT entry lists.  The DSDT is not in these lists; it is
     * referenced from the FADT.
     */
    ctx.tables[0] = ACPI_BASE + madt_off;
    ctx.tables[1] = ACPI_BASE + fadt_off;

    rsdt_entries[0] = (uint32_t)ctx.tables[0];
    rsdt_entries[1] = (uint32_t)ctx.tables[1];
    xsdt_entries[0] = ctx.tables[0];
    xsdt_entries[1] = ctx.tables[1];

    /*
     * Fill the RSDP with the (guest) physical addresses of RSDT and XSDT.
     */
    rsdp->rsdt_addr = (uint32_t)(ACPI_BASE + rsdt_off);
    rsdp->xsdt_addr = (uint64_t)(ACPI_BASE + xsdt_off);
    rsdp->length    = sizeof(*rsdp);

    /*
     * Compute all the checksums.  The RSDP has two: one for the first 20
     * bytes and an extended one for the full 36-byte version-2 RSDP.
     */
    set_checksum((uint8_t *)rsdp, 20, 8);
    set_checksum((uint8_t *)rsdp, sizeof(*rsdp), 32);
    set_checksum((uint8_t *)rsdt, rsdt_size, 9);
    set_checksum((uint8_t *)xsdt, xsdt_size, 9);
    set_checksum((uint8_t *)madt,  madt_size,  9);
    set_checksum((uint8_t *)fadt,  fadt_size,  9);
    set_checksum((uint8_t *)dsdt,  dsdt_size,  9);

    LOG("installed minimal ACPI tables at 0x%08llx", (unsigned long long)ACPI_BASE);
    LOG("  RSDP at 0x%08llx", (unsigned long long)(ACPI_BASE + rsdp_off));
    LOG("  RSDT at 0x%08llx", (unsigned long long)(ACPI_BASE + rsdt_off));
    LOG("  XSDT at 0x%08llx", (unsigned long long)(ACPI_BASE + xsdt_off));
    LOG("  MADT at 0x%08llx (%d local APIC entries)",
        (unsigned long long)(ACPI_BASE + madt_off), vmm->nr_vcpus);
    LOG("  FADT at 0x%08llx, DSDT at 0x%08llx",
        (unsigned long long)(ACPI_BASE + fadt_off),
        (unsigned long long)(ACPI_BASE + dsdt_off));

    return 0;
}
