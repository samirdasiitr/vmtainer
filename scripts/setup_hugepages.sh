#!/bin/bash

# Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
#
# PROPRIETARY AND CONFIDENTIAL.
# Unauthorized copying, reproduction, distribution, or modification of this
# file, via any medium, is strictly prohibited.
# All rights reserved.

set -euo pipefail

# Reserve 64 x 2MB hugepages = 128MB, enough for one 64MB guest + headroom
NR_HP=64

echo "[hugepages] reserving $NR_HP 2MB hugepages..."
echo $NR_HP > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

AVAILABLE=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
if [[ "$AVAILABLE" -lt "$NR_HP" ]]; then
    echo "WARNING: only $AVAILABLE of $NR_HP hugepages available. Check /proc/meminfo."
else
    echo "[hugepages] reserved $AVAILABLE 2MB hugepages"
fi

# Mount hugetlbfs for file-based hugepage allocation if not already mounted
if ! mountpoint -q /dev/hugepages; then
    mkdir -p /dev/hugepages
    mount -t hugetlbfs none /dev/hugepages
    echo "[hugepages] mounted /dev/hugepages"
else
    echo "[hugepages] /dev/hugepages already mounted"
fi
