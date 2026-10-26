#!/bin/bash
# 本地编译 + 仿真校验 SweetAdreno。用法： ./run.sh [0179|0180]
#
# 为什么要这套东西：xnu 只能在 macOS 上编，设备也不在手边，所以这里用一份
# 假的寄存器文件把 SweetAdreno.cpp 原样跑起来，逐条核对 vendor 语义。
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(readlink -f "$HERE/../..")
TAG=${1:-0180}
TREE="$HERE/build/tree"

mkdir -p "$HERE/build"
python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)' "$TREE"
python3 "$HERE/extract.py" "$REPO" "$TREE" "$TAG"
cp "$TREE/iokit/Kernel/arm/SweetAdreno.cpp" "$HERE/build/SweetAdreno.cpp"

clang++ --target=aarch64-none-elf -std=c++17 -O2 -DSWEET -I "$HERE/stubs" -ffreestanding \
        -fno-exceptions -fno-rtti -c "$HERE/build/SweetAdreno.cpp" \
        -o "$HERE/build/SweetAdreno-aarch64.o"
echo "[2/3] aarch64 目标编译通过（保留 dsb sy 内联汇编）"

# x86 上只换掉 dsb 屏障与两处裸指针访问，寄存器逻辑一行不动
sed -e 's|return \*(volatile uint32_t \*)((volatile char \*)base + off);|return sweet_host_rd(base, off);|' \
    -e 's|\*(volatile uint32_t \*)((volatile char \*)base + off) = v;|sweet_host_wr(base, off, v);|' \
    -e 's|__asm__ volatile("dsb sy" ::: "memory");|/* x86 无 dsb：空操作 */|' \
    "$HERE/build/SweetAdreno.cpp" > "$HERE/build/SweetAdreno-sim.cpp"
clang++ -std=c++17 -O1 -DSWEET -I "$HERE/stubs" -I "$HERE/build" -fno-exceptions -fno-rtti \
        "$HERE/host.cpp" -o "$HERE/build/selftest"
echo "[3/3] 仿真台编译通过"
echo

fail=0
run() {
	local out
	out=$(cd "$HERE" && ./build/selftest "$1" "sweetadreno=1,sweetadreno_power=1")
	if grep -qF "$2" <<<"$out"; then
		echo "  ok   $1 → $2"
	else
		echo "  FAIL $1（没等到：$2）"
		fail=1
	fi
}
run happy   "clock sequence returned 0"
run hwcg    "clock sequence returned 0"
run delayed "clock sequence returned 0"
run busy    "update already pending, stopping"
echo
exit $fail
