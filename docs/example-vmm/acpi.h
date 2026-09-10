/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

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
