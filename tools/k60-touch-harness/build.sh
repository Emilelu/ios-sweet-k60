#!/bin/sh
# 把 0219-0223 打到一个临时目录，取出 k60_touch_spi.c，去掉 arm64 的屏障指令，
# 然后用假 MMIO 跑那五步。宿主编译器不认 dmb/dsb，所以只删这三行。
set -e

here=$(cd "$(dirname "$0")" && pwd)
patches="$here/../../xnu/patches"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/src"
# 0219 还顺手改了 bsd/conf/files 和 sweet_ufs.c；这个冒烟台只要那个新文件，
# 所以先把它那一段 awk 出来，免得为了两行上下文去造一棵假 xnu 树。
awk '
/^diff --git / { keep = ($0 ~ /k60_touch_spi\.c/) }
keep' "$patches"/0219-*.patch >"$work/0219-newfile.patch"
patch -s -p1 -d "$work/src" --forward <"$work/0219-newfile.patch" >/dev/null
for p in "$patches"/0220-*.patch "$patches"/0221-*.patch \
    "$patches"/0222-*.patch "$patches"/0223-*.patch; do
	patch -s -p1 -d "$work/src" --forward <"$p" >/dev/null
done

mkdir -p "$work/harness_src"
sed -e 's|__asm__ volatile("dmb oshld" ::: "memory");|(void)0;|' \
    -e 's|__asm__ volatile("dmb oshst" ::: "memory");|(void)0;|' \
    -e 's|__asm__ volatile("dsb osh" ::: "memory");|(void)0;|' \
    "$work/src/bsd/dev/k60_touch_spi.c" >"$work/harness_src/k60_touch_spi.c"

cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
    -I"$here/stub" -I"$work" -DARM64_BOARD_CONFIG_SWEET \
    -o "$work/harness" "$here/harness.c"
"$work/harness"
