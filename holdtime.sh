#!/bin/bash
# Measure how long the device is away from fastboot after booting an image.
IMG=${1:?}; S=ffec9d48
fb() { lsusb 2>/dev/null | grep -qi 18d1:d00d; }
fb || { adb -s $S reboot bootloader >/dev/null 2>&1; for i in $(seq 1 40); do fb && break; sleep 1; done; }
fb || { echo "NO_FB"; exit 2; }
timeout 20 fastboot -s $S boot "$IMG" >/dev/null 2>&1 || { echo "BOOT_FAILED"; exit 3; }
for i in $(seq 1 150); do fb || break; sleep 0.1; done    # hand-off
fb && { echo "NO_HANDOFF"; exit 4; }
t0=$(date +%s%N)
for i in $(seq 1 1200); do fb && { echo "$(( ($(date +%s%N)-t0)/1000000000 ))"; exit 0; }; sleep 0.1; done
echo "TIMEOUT"
