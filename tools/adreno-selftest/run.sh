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

# x86 仿真版：只换访问器、dsb 与 C 链接声明，寄存器逻辑一行不动
python3 "$HERE/simify.py" "$HERE/build/SweetAdreno.cpp" "$HERE/build/SweetAdreno-sim.cpp"
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
# 无 boot-arg：一个 MMIO 都不许碰
run_nobootarg() {
	if grep -qF "ASSERT no-mmio: ok" <<<"$(cd "$HERE" && ./build/selftest happy "")"; then
		echo "  ok   无 boot-arg（完全不碰 MMIO）"
	else
		echo "  FAIL 无 boot-arg 仍然碰了 MMIO"; fail=1
	fi
}

run happy   "clock sequence returned 0"
run hwcg    "clock sequence returned 0"
run delayed "clock sequence returned 0"
run busy    "update already pending, stopping"

# 任意 boot-args 的跑法
runx() {
	local out
	out=$(cd "$HERE" && ./build/selftest "$1" "$2" || true)
	if grep -qF "$3" <<<"$out"; then
		echo "  ok   $1[$2] → $3"
	else
		echo "  FAIL $1[$2]（没等到：$3）"
		fail=1
	fi
}

# 0181 起才有的路径：预检、收尾验证、映射失败、未开开关
if grep -q 'sweet_adreno_preflight' "$HERE/build/SweetAdreno.cpp"; then
	run prepowered "power domain already owned"
	run stuck      "no branch reports running"
	run mapfail    "MMIO mapping failed"
	run_nobootarg
fi

# 0182 起才有 sweetadreno_stage
if grep -q 'sweetadreno_stage' "$HERE/build/SweetAdreno.cpp"; then
	runx happy   "sweetadreno=1,sweetadreno_stage=3" "GMU firmware has not reported FW_INIT OK"
	runx gmulive "sweetadreno=1,sweetadreno_stage=3" "HFI is up"
fi

# M3 预演：真固件 + GMU 引导/失败分支模型（不是内核代码，见 gmu-model.c）
echo "[gmu] 固件与 GMU 引导模型"
if gcc -O1 -o "$HERE/build/gmu-model" "$HERE/gmu-model.c" 2>/dev/null; then
	if FW=${K60_GMU_FW:-$HOME/hackios/gpu-fw/gmu_gen70000.bin}; [ -f "$FW" ]; then
		if "$HERE/build/gmu-model" "$FW" > "$HERE/build/gmu-model.log" 2>&1; then
			echo "  ok   GMU 模型 11 项（成功 + 3 条失败分支）"
		else
			echo "  FAIL GMU 模型，见 build/gmu-model.log"; fail=1
		fi
	else
		echo "  FAIL 找不到固件 $FW"; fail=1
	fi
else
	echo "  FAIL gmu-model.c 编译失败"; fail=1
fi

# M1（只开探测、不开上电）必须零写寄存器
if grep -qF "ASSERT zero-writes: ok" <<<"$(cd "$HERE" && ./build/selftest happy "sweetadreno=1")"; then
	echo "  ok   M1 只读（0 次写寄存器）"
else
	echo "  FAIL M1 路径写了寄存器"; fail=1
fi

echo
exit $fail
