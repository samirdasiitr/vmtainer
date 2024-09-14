#!/bin/bash
# vmtainer_clone.sh -- pull an OCI image, extract rootfs, and run in a VM
# Usage: sudo ./vmtainer_clone.sh <image> [--cmd <command>] [--ip <ip/mask>] [--gw <gateway>] [--name <hostname>]
set -euo pipefail

VMTAINER_DIR=/home/sadas/vmtainer
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

while [ $# -gt 0 ]; do
    case "$1" in
        --cmd)   CMD="$2"; shift 2;;
        --ip)    NET_IP="$2"; shift 2;;
        --gw)    NET_GW="$2"; shift 2;;
        --name)  HOSTNAME="$2"; shift 2;;
        --tap)   TAP_DEV="$2"; shift 2;;
        *)       echo "Unknown arg: $1"; exit 1;;
    esac
done

# Generate a unique clone ID
CLONE_ID="clone-$(date +%s)-$$"
CLONE_DIR="$CLONE_BASE/$CLONE_ID"
ROOTFS="$CLONE_DIR/rootfs"

echo "=== vmtainer clone ==="
echo "  Image:     $IMAGE"
echo "  Clone ID:  $CLONE_ID"
echo "  Rootfs:    $ROOTFS"
echo ""

mkdir -p "$ROOTFS"

# ---------- Step 1: Pull & extract image ----------
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

$VMTAINER_BIN restore "$GOLDEN_SNAP" --config "$CONFIG_FILE" 2>&1
RC=$?

T3=$(date +%s%N)
RESTORE_MS=$(( (T3 - T2) / 1000000 ))

echo ""
echo "=== Clone complete ==="
echo "  Pull+extract: ${PULL_MS}ms"
echo "  VM restore:   ${RESTORE_MS}ms"
echo "  Total:        $(( PULL_MS + RESTORE_MS ))ms"
echo "  Exit code:    $RC"

# Cleanup
rm -rf "$CLONE_DIR"
exit $RC
