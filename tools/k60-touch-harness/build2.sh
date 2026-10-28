#!/bin/sh
# 0240-0243（bsd/dev/k60_touch*.c）的宿主冒烟台。
#
# 源码直接从补丁里取（"new file mode" 的那几段），所以跑的就是要上机的那份，
# 不需要先铺一棵 xnu 树。编译用本目录 stub/ 下的最小内核 API 替身，
# K60_TOUCH_HOST_TEST 让 bsd/sys/k60_touch.h 把 MMIO 读写换成钩子。
#
#   bash build2.sh            跑全部场景
#   HARNESS_ECHO=1 bash build2.sh berlin   把内核日志也打出来
set -e

here=$(cd "$(dirname "$0")" && pwd)
patches="$here/../../xnu/patches"
work=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$work"' EXIT
CC=${CC:-cc}

mkdir -p "$work/bsd/dev" "$work/bsd/sys"
python3 - "$patches" "$work" <<'PY'
import os
import sys

patches, work = sys.argv[1], sys.argv[2]
want = ("k60_touch.c", "k60_touch_pins.c", "k60_touch_rpmh.c",
        "k60_touch_geni.c", "k60_touch_goodix.c", "k60_touch.h")
found = 0
for name in sorted(os.listdir(patches)):
    if not name.startswith(("0240-", "0241-", "0242-", "0243-")):
        continue
    lines = open(os.path.join(patches, name)).read().splitlines(True)
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("diff --git ") and "new file mode" in lines[i + 1]:
            path = line.split(" b/", 1)[1].strip()
            base = os.path.basename(path)
            if base in want:
                body = []
                i += 2
                while i < len(lines) and lines[i].startswith("+"):
                    body.append(lines[i][1:])
                    i += 1
                out = os.path.join(work, path)
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with open(out, "w") as f:
                    f.writelines(body)
                found += 1
                continue
        i += 1
if found != len(want):
    raise SystemExit("extracted %d of %d files" % (found, len(want)))
PY

$CC -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -DK60_TOUCH_HOST_TEST -DKERNEL -DARM64_BOARD_CONFIG_SWEET \
    -I"$here/stub" -I"$work/bsd" \
    -o "$work/harness2" "$here/harness2.c" \
    "$work/bsd/dev/k60_touch_pins.c" \
    "$work/bsd/dev/k60_touch_rpmh.c" \
    "$work/bsd/dev/k60_touch_geni.c" \
    "$work/bsd/dev/k60_touch_goodix.c" \
    "$work/bsd/dev/k60_touch.c"

status=0
if [ "$#" -gt 0 ]; then
	scenarios="$*"
else
	scenarios="default_stage disabled stage0 cmd_db_missing clocks_off \
	    clocks_on geni_timeout berlin gt9xx no_chip"
fi
for s in $scenarios; do
	"$work/harness2" "$s" || status=1
done
exit $status
