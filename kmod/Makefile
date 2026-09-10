# Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
#
# PROPRIETARY AND CONFIDENTIAL.
# Unauthorized copying, reproduction, distribution, or modification of this
# file, via any medium, is strictly prohibited.
# All rights reserved.

# Out-of-tree build for the vmtainer snapshot character device.

obj-m := vmtainer_snap.o
KDIR ?= /lib/modules/$(shell uname -r)/build

ccflags-y := -Wall -Wextra -Wno-unused-parameter

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
