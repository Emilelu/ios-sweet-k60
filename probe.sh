#!/bin/bash
# The ONE way we run shim probes. Handles the three device states itself and
# prints a single line of result, so no more manual mode juggling.
#   probe.sh <img>          ->  "ANDROID <secs>" | "FASTBOOT <secs>" | error
S=ffec9d48
fb()  { lsusb 2>/dev/null | grep -qi 18d1:d00d; }
an()  { adb devices 2>/dev/null | grep -q $S; }   # Xiaomi uses VID 2717, not 18d1, for Android
say() { echo "[probe] $*"; }

ensure_abl_fastboot() {
	for round in 1 2 3; do
		if fb; then
			us=$(timeout 5 fastboot -s $S getvar is-userspace 2>&1 | head -1)
			case "$us" in *"no"*) return 0;; *"yes"*) say "in fastbootd -> switching to ABL"; timeout 10 fastboot -s $S reboot bootloader >/dev/null 2>&1; sleep 6;; *) sleep 2;; esac
		elif an; then
			say "in Android -> reboot bootloader"; adb -s $S reboot bootloader >/dev/null 2>&1
			for i in $(seq 1 40); do fb && break; sleep 1; done
		else
			say "device not on USB"; sleep 3
		fi
	done
	fb || { say "FAILED to reach ABL fastboot"; return 1; }
}

[ -f "${1:?usage: probe.sh <img>}" ] || { say "no such image: $1"; exit 2; }
ensure_abl_fastboot || exit 3
t0=$(date +%s)
if ! timeout 25 fastboot -s $S boot "$1" >/dev/null 2>&1; then
	say "fastboot boot failed once, retrying after a transport reset"
	timeout 10 fastboot -s $S reboot bootloader >/dev/null 2>&1; sleep 6
	ensure_abl_fastboot || exit 3
	timeout 25 fastboot -s $S boot "$1" >/dev/null 2>&1 || { say "BOOT_FAILED"; exit 4; }
fi
# hand-off: the fastboot USB device must disappear
for i in $(seq 1 150); do fb || break; sleep 0.1; done
fb && { say "NO_HANDOFF (payload never started)"; exit 5; }
for i in $(seq 1 2000); do
	if an; then echo "ANDROID $(( $(date +%s)-t0 ))"; exit 0; fi
	if fb; then sleep 2; echo "FASTBOOT $(( $(date +%s)-t0 ))"; exit 0; fi
	sleep 0.1
done
say "TIMEOUT"; exit 6
