#!/bin/sh

# Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
#
# PROPRIETARY AND CONFIDENTIAL.
# Unauthorized copying, reproduction, distribution, or modification of this
# file, via any medium, is strictly prohibited.
# All rights reserved.

# This runs as a shell script (no python in minimal initrd)
echo "HELLO FROM INIT.PY in shared directory!"
echo "hostname: $(hostname 2>/dev/null || echo unknown)"
echo "uptime: $(cat /proc/uptime 2>/dev/null || echo N/A)"
ls -la /share/
