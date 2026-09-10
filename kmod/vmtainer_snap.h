/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

/*
 * vmtainer_snap.h - Userspace/kernel shared ioctl definitions for
 * /dev/vmtainer_snap.
 *
 * This header can be included from both kernel module code (__KERNEL__
 * defined) and userspace tools compiled against glibc/musl.
 */
#ifndef _VMTAINER_SNAP_H
#define _VMTAINER_SNAP_H

#ifdef __KERNEL__
# include <linux/ioctl.h>
# include <linux/types.h>
#else
# include <sys/ioctl.h>
# include <stdint.h>
#endif

#define VMTAINER_IOC_MAGIC	'V'

#define VMTAINER_SNAP_ALLOC	 _IOW(VMTAINER_IOC_MAGIC, 1, unsigned long)
#define VMTAINER_SNAP_INFO	 _IOR(VMTAINER_IOC_MAGIC, 2, struct vmtainer_snap_info)

struct vmtainer_snap_info {
	unsigned long size;
	unsigned long phys_addr;	/* always 0 for vmalloc-backed memory */
};

#endif /* _VMTAINER_SNAP_H */
