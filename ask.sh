#!/bin/bash
# Verified binary question: PASS = back in fastboot, FAIL = Android, UNKNOWN otherwise.
IMG=${1:?}; NAME=${2:-probe}; S=ffec9d48
have_fb() { lsusb 2>/dev/null | grep -qi 18d1:d00d; }
have_an() { lsusb 2>/dev/null | grep -qi 18d1:4ee7; }
for i in $(seq 1 20); do have_fb && break; timeout 4 fastboot devices 2>/dev/null | grep -q $S && break
  have_an && { adb -s $S reboot bootloader >/dev/null 2>&1; }; sleep 2; done
timeout 5 fastboot devices 2>/dev/null | grep -q $S || { echo "$NAME=NO_FASTBOOT"; exit 2; }
out=$(timeout 20 fastboot -s $S boot "$IMG" 2>&1); echo "$out" | grep -q "^Booting" || { echo "$NAME=BOOT_CMD_FAILED"; exit 3; }
for i in $(seq 1 100); do have_fb || break; sleep 0.1; done
have_fb && { echo "$NAME=NO_HANDOFF"; exit 4; }     # payload never took over
for i in $(seq 1 300); do
  have_fb && { echo "$NAME=PASS"; exit 0; }
  have_an && { echo "$NAME=FAIL"; exit 1; }
  sleep 0.1
done
echo "$NAME=UNKNOWN"
