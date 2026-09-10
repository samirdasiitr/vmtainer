#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$ROOT/build/vmm"

echo "[build_vmm] Compiling vmtainer (vmm.cpp + virtio.cpp)..."
g++ -O2 -std=c++17 -Wall -Wextra \
    "$ROOT/src/vmm.cpp" \
    "$ROOT/src/virtio.cpp" \
    -o "$ROOT/build/vmm/vmtainer" \
    -pthread

echo "[build_vmm] Successfully built $ROOT/build/vmm/vmtainer"
