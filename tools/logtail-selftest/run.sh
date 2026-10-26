#!/bin/bash
# Host-side verification of sweet_log_dump()'s backward tail reader.
# No device, no network, no kernel build: one cc invocation and one run.
#
#   ./run.sh                     all cases, exit 0 when everything matches
#   ./run.sh --list              list the cases
#   ./run.sh --case=wrap_head_777
#   ./run.sh --sabotage=boff     inject a known bug; must fail
#   ./run.sh --fuzz=2000         more randomised streams (LOGTAIL_FUZZ=N too)
set -uo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-cc}
FUZZ=${LOGTAIL_FUZZ:-150}

mkdir -p "$HERE/build"
echo "[1/2] build (host cc, no kernel headers)"
if ! "$CC" -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
	-o "$HERE/build/logtail" "$HERE/logtail.c"; then
	echo "      build failed" >&2
	exit 2
fi
echo "      $("$HERE/build/logtail" --list | wc -l) cases, $(du -h "$HERE/build/logtail" | cut -f1) binary"

echo "[2/2] run"
"$HERE/build/logtail" --fuzz="$FUZZ" "$@"
rc=$?
if [ $rc -ne 0 ]; then
	echo
	echo "run.sh: FAILED (exit $rc)" >&2
	exit $rc
fi
exit 0
