#!/bin/bash
# bench_restore_only.sh -- Benchmark N parallel VM restores
# Measures ONLY the VMM restore time (KVM init + snapshot copy + virtiofsd).
# Kills the VM immediately after restore completes (doesn't wait for guest).
# Usage: sudo ./bench_restore_only.sh [count] [mode]
#   mode=full:      with virtiofsd (default)
#   mode=novfs:     without virtiofsd (no rootfs needed)
set -euo pipefail

COUNT=${1:-100}
MODE=${2:-full}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VMTAINER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VMTAINER=$VMTAINER_DIR/build/vmm/vmtainer
GOLDEN=$VMTAINER_DIR/images/golden.snap
RESULTS_DIR=/tmp/vmtainer-restonly-$$
ROOTFS=/tmp/vmtainer-nginx-rootfs

echo "=== vmtainer restore-only benchmark ==="
echo "  Count:   $COUNT"
echo "  Mode:    $MODE"
echo "  Results: $RESULTS_DIR"
echo ""

mkdir -p "$RESULTS_DIR"

# Pre-warm snapshot into page cache
cat "$GOLDEN" > /dev/null

echo "[1/3] Launching $COUNT VMs in parallel..."
T_START=$(date +%s%N)

for i in $(seq 0 $((COUNT - 1))); do
    (
        T_VM_START=$(date +%s%N)

        if [ "$MODE" = "novfs" ]; then
            # No virtiofsd: just restore and let the VM try to run
            # Use timeout to kill stuck VMs after 10s
            OUTPUT=$(timeout 10 $VMTAINER restore "$GOLDEN" 2>&1 || true)
        else
            # With virtiofsd: create a minimal config pointing to the rootfs
            cat > "$RESULTS_DIR/config_$i.json" <<EOF
{
    "hostname": "bench-$i",
    "rootfs": "$ROOTFS",
    "entrypoint": "/bin/true"
}
EOF
            OUTPUT=$(timeout 10 $VMTAINER restore "$GOLDEN" --config "$RESULTS_DIR/config_$i.json" 2>&1 || true)
        fi

        T_VM_END=$(date +%s%N)
        VM_MS=$(( (T_VM_END - T_VM_START) / 1000000 ))
        RESTORE_LINE=$(echo "$OUTPUT" | grep "restored in" || echo "FAILED")
        echo "${VM_MS}|${RESTORE_LINE}" > "$RESULTS_DIR/result_$i.txt"
    ) &
done

echo "  Waiting for all $COUNT VMs..."
wait
T_END=$(date +%s%N)
TOTAL_MS=$(( (T_END - T_START) / 1000000 ))

echo "[2/3] Collecting results..."
echo ""

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
        v=$1+0; sum+=v; n++;
        if(v<min) min=v;
        if(v>max) max=v;
        vals[n]=v;
    }
    END {
        avg=sum/n;
        for(i=1;i<=n;i++) sorted[i]=vals[i];
        for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(sorted[i]>sorted[j]){t=sorted[i];sorted[i]=sorted[j];sorted[j]=t}
        p50=sorted[int(n*0.5)+1];
        p90=sorted[int(n*0.9)+1];
        p99=sorted[int(n*0.99)+1];
        printf "  %-20s min=%.1f  avg=%.1f  p50=%.1f  p90=%.1f  p99=%.1f  max=%.1f  (ms)\n", name":", min, avg, p50, p90, p99, max
    }'
}

echo "================================================================"
echo "  RESTORE-ONLY BENCHMARK  (N=$COUNT, mode=$MODE)"
echo "================================================================"
echo ""
echo "  Success: $SUCCESSES / $COUNT"
echo "  Failed:  $FAILURES / $COUNT"
echo "  Wall clock (all parallel): ${TOTAL_MS}ms"
echo ""
calc_stats RESTORE_TIMES "Total restore"
calc_stats SNAP_TIMES "  Snapshot copy"
calc_stats KVM_TIMES "  KVM init"
if [ "$MODE" = "full" ]; then
    calc_stats VFS_TIMES "  virtiofsd"
fi
calc_stats WALL_TIMES "Wall time"
echo ""
echo "  Throughput: $(echo "$COUNT $TOTAL_MS" | awk '{printf "%.1f VMs/sec", $1/($2/1000.0)}') (parallel)"
echo "================================================================"

rm -rf "$RESULTS_DIR"
