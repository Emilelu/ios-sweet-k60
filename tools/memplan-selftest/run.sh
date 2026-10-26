#!/bin/bash
# Local verification for the K60 memory plan. No device, no macOS.
#
#   ./run.sh          derive + table + all assertions
#
# Layers, same idea as ../adreno-selftest:
#   1. memplan.py re-derives the map from dump/live.dts + dump/iomem.txt and
#      refuses to go on if the checked-in k60-physmap.json has drifted.
#   2. sim.c models arm64 pmap_startup()'s page walk and prints which pages end
#      up on a free list.
#   3. check.py turns that into pass/fail: no freed page outside iomem System
#      RAM, every carve-out notched, panic log placement, and three negative
#      controls that must fail.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/build"

echo "[1/3] derive the physical map"
python3 "$HERE/memplan.py" --verify

echo "[2/3] build the page-walk model"
cc -O2 -Wall -Wextra -o "$HERE/build/sim" "$HERE/sim.c"
echo "      $(du -h "$HERE/build/sim" | cut -f1) x86 binary"

echo "[3/3] assertions"
python3 "$HERE/check.py"
echo
python3 "$HERE/memplan.py" --plans
