#!/bin/bash
set -euo pipefail
ROOT=/home/sadas/vmtainer
mkdir -p "$ROOT/build/vmm"
g++ -O2 -std=c++17 -Wall -Wextra \
    "$ROOT/src/vmm.cpp" \
    -o "$ROOT/build/vmm/vmtainer" \
    -pthread
