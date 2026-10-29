#!/bin/sh
# 0240-0251（bsd/dev/k60_touch*.c）的宿主冒烟台。
#
# 源码直接从补丁里取，所以跑的就是要上机的那份，不需要先铺一棵 xnu 树。
# 0240-0243 是"新建文件"的补丁，0244 起是对这些文件的小改；两类都用
# git apply 打在临时树的 bsd/ 下（打不上就退出——上游构建脚本会静默跳过
# 打不上的补丁，那正是这个冒烟台要挡住的情况）。
# 编译用本目录 stub/ 下的最小内核 API 替身；K60_TOUCH_HOST_TEST 让
# bsd/sys/k60_touch.h 把 MMIO 读写换成钩子，harness2.c 再用这些钩子模拟
# GENI 引擎、TLMM 引脚页、RSC 槽位、一个 /k60-touch 节点和一颗 Goodix。
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
(
    cd "$work"
    git init -q .
    for p in "$patches"/02[4-5][0-9]-*.patch; do
        git apply -q \
            --exclude='bsd/dev/k60_touch_spi.c' \
            --exclude='bsd/conf/files' \
            --exclude='bsd/dev/sweet_ufs.c' \
            --include='bsd/dev/k60_touch*' \
            --include='bsd/sys/k60_touch.h' \
            "$p" || { echo "harness: $p does not apply" >&2; exit 1; }
    done
)
for f in bsd/sys/k60_touch.h bsd/dev/k60_touch.c bsd/dev/k60_touch_pins.c \
         bsd/dev/k60_touch_rpmh.c bsd/dev/k60_touch_geni.c \
         bsd/dev/k60_touch_goodix.c bsd/dev/k60_touch_dt.c; do
    test -f "$work/$f" || { echo "harness: $f missing after patching" >&2; exit 1; }
done

$CC -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-builtin \
    -DK60_TOUCH_HOST_TEST -DKERNEL -DARM64_BOARD_CONFIG_SWEET \
    -I"$here/stub" -I"$work/bsd" \
    -o "$work/harness2" "$here/harness2.c" \
    "$work/bsd/dev/k60_touch_pins.c" \
    "$work/bsd/dev/k60_touch_rpmh.c" \
    "$work/bsd/dev/k60_touch_geni.c" \
    "$work/bsd/dev/k60_touch_goodix.c" \
    "$work/bsd/dev/k60_touch_dt.c" \
    "$work/bsd/dev/k60_touch.c"

status=0
if [ "$#" -gt 0 ]; then
	scenarios="$*"
else
	scenarios="default_stage disabled stage0 cmd_db_missing clocks_off \
	    clocks_on geni_timeout berlin gt9xx no_chip \
	    dt_node dt_missing rpmhoff rscprobe_refused id_readback"
fi
for s in $scenarios; do
	"$work/harness2" "$s" || status=1
done
exit $status
