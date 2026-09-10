#ifndef ACPI_H
#define ACPI_H

#include "vmm.h"

/*
 * Build a minimal but well-formed set of ACPI tables and place them in the
 * reserved low memory that Linux scans (starting at 0xE0000).  The tables
 * demonstrate RSDP, RSDT, XSDT, MADT, FADT, and DSDT placement and linkage.
 */
int acpi_install(struct vmm *vmm);

#endif /* ACPI_H */
