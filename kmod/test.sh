#!/bin/bash
# test.sh - Build, load, benchmark, and unload the vmtainer snapshot module.
#
# Usage: ./test.sh [path/to/golden.snap]
# Default snapshot: /home/sadas/vmtainer/images/golden.snap

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

SNAP="${1:-/home/sadas/vmtainer/images/golden.snap}"

echo "=== Building kernel module ==="
make clean 2>/dev/null || true
make

echo "=== Loading vmtainer_snap.ko ==="
sudo insmod ./vmtainer_snap.ko

# devtmpfs should create /dev/vmtainer_snap; give it a moment and fix
# permissions in case udev is not running or the mode was restricted.
sleep 0.5
if [ ! -c /dev/vmtainer_snap ]; then
    echo "Error: /dev/vmtainer_snap did not appear" >&2
    sudo rmmod vmtainer_snap || true
    exit 1
fi
sudo chmod 666 /dev/vmtainer_snap 2>/dev/null || true
ls -la /dev/vmtainer_snap

echo "=== Building snap_bench ==="
gcc -O2 -D_GNU_SOURCE -o snap_bench snap_bench.c

echo "=== Running benchmark with ${SNAP} ==="
sudo ./snap_bench "${SNAP}"

echo "=== Unloading module ==="
sudo rmmod vmtainer_snap

echo "=== Done ==="
