#!/bin/bash

# Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
#
# PROPRIETARY AND CONFIDENTIAL.
# Unauthorized copying, reproduction, distribution, or modification of this
# file, via any medium, is strictly prohibited.
# All rights reserved.

# vmtainer_clone.sh -- pull an OCI image, extract rootfs, and run in a VM
# Usage: sudo ./vmtainer_clone.sh <image> [--cmd <command>] [--ip <ip/mask>] [--gw <gateway>] [--name <hostname>]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VMTAINER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GOLDEN_SNAP=$VMTAINER_DIR/images/golden.snap
VMTAINER_BIN=$VMTAINER_DIR/build/vmm/vmtainer
CLONE_BASE=/tmp/vmtainer-clones

IMAGE="${1:-}"
if [ -z "$IMAGE" ]; then
    echo "Usage: $0 <image> [--cmd <command>] [--ip <ip/mask>] [--gw <gw>] [--name <name>]"
    exit 1
fi
shift

# Defaults
CMD=""
NET_IP="10.0.0.2/24"
NET_GW="10.0.0.1"
HOSTNAME="vmtainer"
TAP_DEV="tap0"
PRE_ROOTFS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --cmd)     CMD="$2"; shift 2;;
        --ip)      NET_IP="$2"; shift 2;;
        --gw)      NET_GW="$2"; shift 2;;
        --name)    HOSTNAME="$2"; shift 2;;
        --tap)     TAP_DEV="$2"; shift 2;;
        --rootfs)  PRE_ROOTFS="$2"; shift 2;;
        *)         echo "Unknown arg: $1"; exit 1;;
    esac
done

# Generate a unique clone ID
CLONE_ID="clone-$(date +%s)-$$"
CLONE_DIR="$CLONE_BASE/$CLONE_ID"
if [ -n "$PRE_ROOTFS" ]; then
    ROOTFS="$PRE_ROOTFS"
else
    ROOTFS="$CLONE_DIR/rootfs"
fi
mkdir -p "$CLONE_DIR"

echo "=== vmtainer clone ==="
echo "  Image:     $IMAGE"
echo "  Clone ID:  $CLONE_ID"
echo "  Rootfs:    $ROOTFS"
echo ""

# ---------- Step 1: Pull & extract image ----------
if [ -n "$PRE_ROOTFS" ]; then
    PULL_MS=0
    echo "[1/4] Using pre-extracted rootfs (skipped pull & extract)"
else
    mkdir -p "$ROOTFS"
    T0=$(date +%s%N)
    echo "[1/4] Pulling image..."

    # Use docker to create a container and export its filesystem
    CONTAINER_ID=$(docker create "$IMAGE" /bin/true 2>/dev/null)
    if [ -z "$CONTAINER_ID" ]; then
        echo "  docker pull first..."
        docker pull "$IMAGE" >/dev/null 2>&1
        CONTAINER_ID=$(docker create "$IMAGE" /bin/true 2>/dev/null)
    fi

    echo "[2/4] Extracting rootfs..."
    docker export "$CONTAINER_ID" | tar -xf - -C "$ROOTFS" 2>/dev/null
    docker rm "$CONTAINER_ID" >/dev/null 2>&1

    T1=$(date +%s%N)
    PULL_MS=$(( (T1 - T0) / 1000000 ))
    echo "  Extracted to $ROOTFS (${PULL_MS}ms)"
fi

# Get image metadata for default CMD if not overridden
if [ -z "$CMD" ]; then
    # Try to get the CMD/ENTRYPOINT from docker inspect
    INSPECT=$(docker inspect "$IMAGE" 2>/dev/null || echo '[]')
    IMG_ENTRYPOINT=$(echo "$INSPECT" | jq -r '.[0].Config.Entrypoint // empty | join(" ")' 2>/dev/null || true)
    IMG_CMD=$(echo "$INSPECT" | jq -r '.[0].Config.Cmd // empty | join(" ")' 2>/dev/null || true)
    if [ -n "$IMG_ENTRYPOINT" ]; then
        CMD="$IMG_ENTRYPOINT $IMG_CMD"
    elif [ -n "$IMG_CMD" ]; then
        CMD="$IMG_CMD"
    else
        CMD="/bin/sh"
    fi
    echo "  Entrypoint: $CMD"
fi

# ---------- Step 2: Write config ----------
echo "[3/4] Preparing config..."

CONFIG_FILE="$CLONE_DIR/config.json"
cat > "$CONFIG_FILE" <<ENDJSON
{
    "hostname": "$HOSTNAME",
    "rootfs": "$ROOTFS",
    "net": {
        "tap": "$TAP_DEV",
        "ip": "$NET_IP",
        "gateway": "$NET_GW",
        "mac": "52:54:00:12:34:56"
    },
    "entrypoint": "$CMD",
    "env": {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "TERM": "xterm"
    }
}
ENDJSON

# ---------- Step 3: Restore snapshot ----------
echo "[4/4] Restoring VM from snapshot..."
T2=$(date +%s%N)

TMP_LOG=$(mktemp)
$VMTAINER_BIN restore "$GOLDEN_SNAP" --config "$CONFIG_FILE" 2>&1 | tee "$TMP_LOG"
RC=${PIPESTATUS[0]}

T3=$(date +%s%N)
TOTAL_VM_MS=$(( (T3 - T2) / 1000000 ))
SNAP_RESTORE_MS=$(grep -oP 'restored in \K[0-9.]+' "$TMP_LOG" | head -1 || echo "")
ENTRYPOINT_MS=$(grep -oP 'TIME TO START OF ENTRYPOINT EXECUTION: \K[0-9.]+' "$TMP_LOG" | head -1 || echo "")
rm -f "$TMP_LOG"

echo ""
echo "=== Clone complete ==="
echo "  Pull+extract:       ${PULL_MS}ms"
if [ -n "$SNAP_RESTORE_MS" ]; then
echo "  Snapshot restore:   ${SNAP_RESTORE_MS}ms"
fi
if [ -n "$ENTRYPOINT_MS" ]; then
echo "  Time to entrypoint: ${ENTRYPOINT_MS}ms"
fi
echo "  Total VM runtime:   ${TOTAL_VM_MS}ms"
echo "  Total E2E time:     $(( PULL_MS + TOTAL_VM_MS ))ms"
echo "  Exit code:          $RC"

# Cleanup
if [ -z "$PRE_ROOTFS" ]; then
    rm -rf "$CLONE_DIR"
else
    rm -f "$CONFIG_FILE" "$ROOTFS/.entrypoint" "$ROOTFS/.vmconfig"
    rmdir "$CLONE_DIR" 2>/dev/null || true
fi
exit $RC
