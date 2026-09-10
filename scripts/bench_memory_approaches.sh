#!/bin/bash

# Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
#
# PROPRIETARY AND CONFIDENTIAL.
# Unauthorized copying, reproduction, distribution, or modification of this
# file, via any medium, is strictly prohibited.
# All rights reserved.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VMTAINER="$ROOT/build/vmm/vmtainer"
SNAP="$ROOT/images/golden_512.snap"
ROOTFS="$ROOT/test_rootfs"
RUNS=5

if [ "$EUID" -ne 0 ]; then
    echo "This benchmark requires root (sudo) for userfaultfd access."
    exit 1
fi

echo "================================================================================"
echo "         VMTAINER MEMORY EXPANSION CLONING BENCHMARK (512MB RAM)"
echo "================================================================================"
echo "Snapshot: $SNAP (512MB total RAM, ~36.4MB dirty pages)"
echo "Runs per approach: $RUNS"
echo ""

# Ensure mem_bench binary is built
if [ ! -f "$ROOTFS/bin/mem_bench" ]; then
    echo "Building $ROOTFS/bin/mem_bench..."
    mkdir -p "$ROOTFS/bin"
    gcc -O3 -static "$ROOT/scripts/mem_bench.c" -o "$ROOTFS/bin/mem_bench"
fi

# Pre-warm snapshot into page cache
cat "$SNAP" > /dev/null

run_approach() {
    local name="$1"
    local flags="$2"
    local entrypoint="$3"

    declare -a vmm_restores=()
    declare -a snap_times=()
    declare -a guest_inits=()
    declare -a entrypoint_times=()
    declare -a mem_latencies=()
    declare -a mem_bws=()
    declare -a uffd_faults=()
    declare -a total_walls=()

    echo "--------------------------------------------------------------------------------"
    echo "Testing Approach: $name ($flags)"
    echo "Workload: $entrypoint"
    echo "--------------------------------------------------------------------------------"

    for i in $(seq 1 $RUNS); do
        local out
        out=$($VMTAINER restore "$SNAP" --share "$ROOTFS" $flags --entrypoint "$entrypoint" 2>&1)

        # Parse metrics
        local vmm_restore snap_t guest_init entry_start mem_lat mem_bw faults total_wall

        vmm_restore=$(echo "$out" | grep -oP 'restored in \K[\d.]+' || echo "0")
        snap_t=$(echo "$out" | grep -oP 'snap=\K[\d.]+' || echo "0")
        entry_start=$(echo "$out" | grep -oP 'TIME TO START OF ENTRYPOINT EXECUTION: \K[\d.]+' || echo "0")
        guest_init=$(echo "$out" | grep -oP 'Guest mount & init to entrypoint:\s+\K[\d.]+' || echo "0")
        mem_lat=$(echo "$out" | grep -oP 'Result: \d+ MB in \K[\d.]+' || echo "0")
        mem_bw=$(echo "$out" | grep -oP 'throughput: \K[\d.]+' || echo "0")
        faults=$(echo "$out" | grep -oP 'uffd page faults handled: \K\d+' || echo "0")
        total_wall=$(echo "$out" | grep -oP '\[\+\s*\K[\d.]+(?=ms\] VMTAINER: entrypoint exited)' || echo "0")

        vmm_restores+=("$vmm_restore")
        snap_times+=("$snap_t")
        guest_inits+=("$guest_init")
        entrypoint_times+=("$entry_start")
        mem_latencies+=("$mem_lat")
        mem_bws+=("$mem_bw")
        uffd_faults+=("$faults")
        total_walls+=("$total_wall")

        printf "  Run %d: VMM Restore=%5.2fms (snap=%4.1fms) | Guest Init=%4.2fms | Entrypoint Start=%5.2fms | Mem Lat=%5.1fms (%5.1f MB/s) | Faults=%d | Wall=%5.1fms\n" \
               "$i" "$vmm_restore" "$snap_t" "$guest_init" "$entry_start" "$mem_lat" "$mem_bw" "$faults" "$total_wall"
    done

    # Calculate averages
    calc_avg() {
        local -n arr=$1
        printf '%s\n' "${arr[@]}" | awk '{sum+=$1; n++} END {if (n>0) printf "%.2f", sum/n; else printf "0"}'
    }

    echo ""
    echo "  >> AVERAGE RESULTS for $name:"
    echo "     VMM Restore Latency:      $(calc_avg vmm_restores) ms"
    echo "       - Snapshot Memory Copy: $(calc_avg snap_times) ms"
    echo "     Guest Init to Entrypoint: $(calc_avg guest_inits) ms"
    echo "     Time to Entrypoint Start: $(calc_avg entrypoint_times) ms"
    echo "     Memory Alloc/Write 200MB: $(calc_avg mem_latencies) ms"
    echo "     Memory Expansion Bandwidth: $(calc_avg mem_bws) MB/s"
    echo "     UFFD Page Faults Handled: $(calc_avg uffd_faults)"
    echo "     Total Container Wall Time: $(calc_avg total_walls) ms"
    echo ""
}

# 1. Approach A: Full Lazy userfaultfd
run_approach "Approach A: Full Lazy userfaultfd" "" "/bin/mem_bench 200"

# 2. Approach B: Midway with userfaultfd on new memory
run_approach "Approach B: Midway with userfaultfd" "--midway-uffd" "/bin/mem_bench 200"

# 3. Approach C: Midway with Native Kernel Demand Paging
run_approach "Approach C: Midway with Native Demand Paging" "--no-uffd" "/bin/mem_bench 200"

echo "================================================================================"
echo "Benchmark completed successfully."
echo "================================================================================"
