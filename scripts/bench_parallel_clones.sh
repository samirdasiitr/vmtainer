#!/bin/bash

# Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
#
# PROPRIETARY AND CONFIDENTIAL.
# Unauthorized copying, reproduction, distribution, or modification of this
# file, via any medium, is strictly prohibited.
# All rights reserved.

# bench_parallel_clones.sh -- Benchmark N parallel VM clones from golden snapshot
# Usage: sudo ./bench_parallel_clones.sh [count] [rootfs_path]
set -euo pipefail

COUNT=${1:-100}
ROOTFS=${2:-/tmp/alpine-rootfs}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VMTAINER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VMTAINER=$VMTAINER_DIR/build/vmm/vmtainer
GOLDEN=$VMTAINER_DIR/images/golden.snap
RESULTS_DIR=/tmp/vmtainer-bench-$$
SUBNET="10.100"

echo "=== vmtainer parallel clone benchmark ==="
echo "  Count:   $COUNT"
echo "  Rootfs:  $ROOTFS"
echo "  Snapshot: $GOLDEN"
echo "  Results: $RESULTS_DIR"
echo ""

mkdir -p "$RESULTS_DIR"

# Clean up any leftover TAP devices
for i in $(seq 0 $((COUNT + 10))); do
    ip link del "vtap$i" 2>/dev/null || true
done

# Pre-warm the snapshot into page cache
cat "$GOLDEN" > /dev/null

echo "[1/4] Creating $COUNT TAP devices..."
T_TAP_START=$(date +%s%N)
for i in $(seq 0 $((COUNT - 1))); do
    ip tuntap add dev "vtap$i" mode tap 2>/dev/null || true
    # Each VM gets IP 10.100.X.Y where X=i/250, Y=2+(i%250)
    OCTET3=$(( i / 250 ))
    OCTET4=$(( 2 + (i % 250) ))
    ip addr add "${SUBNET}.${OCTET3}.$((OCTET4 - 1))/30" dev "vtap$i" 2>/dev/null || true
    ip link set "vtap$i" up
done
T_TAP_END=$(date +%s%N)
TAP_MS=$(( (T_TAP_END - T_TAP_START) / 1000000 ))
echo "  TAP setup: ${TAP_MS}ms"

echo "[2/4] Preparing $COUNT configs..."
for i in $(seq 0 $((COUNT - 1))); do
    OCTET3=$(( i / 250 ))
    OCTET4=$(( 2 + (i % 250) ))
    GW="${SUBNET}.${OCTET3}.$((OCTET4 - 1))"
    IP="${SUBNET}.${OCTET3}.${OCTET4}/30"
    MAC=$(printf "52:54:00:%02x:%02x:%02x" $((i / 256 / 256 % 256)) $((i / 256 % 256)) $((i % 256)))
    cat > "$RESULTS_DIR/config_$i.json" <<EOF
{
    "hostname": "clone-$i",
    "rootfs": "$ROOTFS",
    "net": {
        "tap": "vtap$i",
        "ip": "$IP",
        "gateway": "$GW",
        "mac": "$MAC"
    },
    "entrypoint": "/bin/true",
    "env": { "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" }
}
EOF
done

echo "[3/4] Launching $COUNT VMs in parallel..."
T_LAUNCH_START=$(date +%s%N)

# Launch all VMs in parallel, capture timing to individual files
for i in $(seq 0 $((COUNT - 1))); do
    (
        T_VM_START=$(date +%s%N)
        OUTPUT=$($VMTAINER restore "$GOLDEN" --config "$RESULTS_DIR/config_$i.json" 2>&1)
        T_VM_END=$(date +%s%N)
        VM_MS=$(( (T_VM_END - T_VM_START) / 1000000 ))
        RESTORE_LINE=$(echo "$OUTPUT" | grep "restored in" || echo "FAILED")
        echo "${VM_MS}|${RESTORE_LINE}" > "$RESULTS_DIR/result_$i.txt"
    ) &
done

echo "  Waiting for all $COUNT VMs to complete..."
wait
T_LAUNCH_END=$(date +%s%N)
TOTAL_MS=$(( (T_LAUNCH_END - T_LAUNCH_START) / 1000000 ))

echo "[4/4] Collecting results..."
echo ""

# Parse results
SUCCESSES=0
FAILURES=0
declare -a RESTORE_TIMES
declare -a WALL_TIMES
declare -a SNAP_TIMES
declare -a KVM_TIMES
declare -a VFS_TIMES

for i in $(seq 0 $((COUNT - 1))); do
    if [ -f "$RESULTS_DIR/result_$i.txt" ]; then
        LINE=$(cat "$RESULTS_DIR/result_$i.txt")
        WALL=$(echo "$LINE" | cut -d'|' -f1)
        WALL_TIMES+=("$WALL")
        
        RESTORE=$(echo "$LINE" | grep -oP 'restored in [\d.]+ms' | grep -oP '[\d.]+' || echo "")
        if [ -n "$RESTORE" ]; then
            SUCCESSES=$((SUCCESSES + 1))
            RESTORE_TIMES+=("$RESTORE")
            
            SNAP=$(echo "$LINE" | grep -oP 'snap=[\d.]+ms' | grep -oP '[\d.]+' || echo "0")
            KVM=$(echo "$LINE" | grep -oP 'kvm_init=[\d.]+ms' | grep -oP '[\d.]+' || echo "0")
            VFS=$(echo "$LINE" | grep -oP 'virtiofsd=[\d.]+ms' | grep -oP '[\d.]+' || echo "0")
            SNAP_TIMES+=("$SNAP")
            KVM_TIMES+=("$KVM")
            VFS_TIMES+=("$VFS")
        else
            FAILURES=$((FAILURES + 1))
        fi
    else
        FAILURES=$((FAILURES + 1))
    fi
done

# Calculate stats using awk
calc_stats() {
    local -n arr=$1
    local name=$2
    if [ ${#arr[@]} -eq 0 ]; then
        echo "  $name: no data"
        return
    fi
    printf '%s\n' "${arr[@]}" | awk -v name="$name" '
    BEGIN { min=99999; max=0; sum=0; n=0 }
    {
        v=$1+0;
        sum+=v; n++;
        if(v<min) min=v;
        if(v>max) max=v;
        vals[n]=v;
    }
    END {
        avg=sum/n;
        # Sort for percentiles
        for(i=1;i<=n;i++) sorted[i]=vals[i];
        for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(sorted[i]>sorted[j]){t=sorted[i];sorted[i]=sorted[j];sorted[j]=t}
        p50=sorted[int(n*0.5)+1];
        p90=sorted[int(n*0.9)+1];
        p99=sorted[int(n*0.99)+1];
        printf "  %-20s min=%.1f  avg=%.1f  p50=%.1f  p90=%.1f  p99=%.1f  max=%.1f  (ms)\n", name":", min, avg, p50, p90, p99, max
    }'
}

echo "================================================================"
echo "  PARALLEL CLONE BENCHMARK RESULTS  (N=$COUNT)"
echo "================================================================"
echo ""
echo "  Success: $SUCCESSES / $COUNT"
echo "  Failed:  $FAILURES / $COUNT"
echo "  Wall clock (all $COUNT parallel): ${TOTAL_MS}ms"
echo ""
calc_stats RESTORE_TIMES "Total restore"
calc_stats SNAP_TIMES "  Snapshot copy"
calc_stats KVM_TIMES "  KVM init"
calc_stats VFS_TIMES "  virtiofsd"
calc_stats WALL_TIMES "Wall time"
echo ""
echo "  Throughput: $(echo "$COUNT $TOTAL_MS" | awk '{printf "%.1f VMs/sec", $1/($2/1000.0)}') (parallel)"
echo "================================================================"

# Save full results
echo ""
echo "Detailed per-VM results saved to: $RESULTS_DIR/"

# Cleanup TAP devices
echo ""
echo "Cleaning up..."
for i in $(seq 0 $((COUNT - 1))); do
    ip link del "vtap$i" 2>/dev/null || true
done
echo "Done."
