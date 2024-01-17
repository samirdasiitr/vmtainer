#!/bin/sh
# This runs as a shell script (no python in minimal initrd)
echo "HELLO FROM INIT.PY in shared directory!"
echo "hostname: $(hostname 2>/dev/null || echo unknown)"
echo "uptime: $(cat /proc/uptime 2>/dev/null || echo N/A)"
ls -la /share/
