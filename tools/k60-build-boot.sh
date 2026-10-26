#!/bin/bash
# Build a boot image from a kernel, always with a freshly generated ADT.
#
#   tools/k60-build-boot.sh <kernel> [out.img]
#
# Refuses to build if the ADT it just generated does not advertise the panel-free
# 0xb4000000 + 703 MB layout: artifacts/k60.adt holds the old 0x80000000 layout,
# using it makes the kernel see 873 MB of "RAM" and every daemon crashes at
# random (2026-10-25, two rounds lost).
set -euo pipefail
KERNEL=$(readlink -f "${1:?usage: k60-build-boot.sh <kernel> [out.img]}")
OUT=$(readlink -m "${2:-$HOME/hackios/xnu-next.img}")
HACKIO=$HOME/hackios
ADT=/tmp/k60-adt-$$.bin
PAYLOAD=/tmp/k60-payload-$$.bin

cleanup() { :; }

ADT_PHYS_BASE=0xb4000000 ADT_MEM_SIZE=0x2c000000 \
    python3 "$HACKIO/ios-sweet/tools/mkadt.py" "$ADT" --k60 >/dev/null

python3 - "$ADT" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
base = struct.pack('>Q', 0xb4000000)[::-1]
size = struct.pack('>Q', 0x2bf00000)[::-1]
if base not in d or size not in d:
    sys.exit('ADT check failed: expected dram-base 0xb4000000 and dram-size 0x2bf00000')

print('ADT ok: dram 0xb4000000 + 0x2bf00000 (703 MiB), panel buffer inside it')
PY

# K60_CMDLINE_EXTRA：追加到标准 cmdline 后（例如 "sweetadreno=1"），
# 不要覆盖已有参数，只做空格拼接。
EXTRA=${K60_CMDLINE_EXTRA:-}
python3 "$HACKIO/ios-sweet/tools/mkpayload.py" "$KERNEL" "$ADT" "$PAYLOAD" \
    --cmdline "debug=0x8 keepsyms=1 cpus=8 -noprogress serial=1 sweet_notests=1 rd=ufsroot0 ufsroot=userdata ufsrw=1 amfi_get_out_of_my_way=1 cs_enforcement_disable=1 amfi_allow_any_signature=1 ufslogsecs=5${EXTRA:+ $EXTRA}" >/dev/null
[ -n "$EXTRA" ] && echo "cmdline extra: $EXTRA"

cd "$HACKIO/ios-sweet/shim"
HDRVER=4 CFLAGS_EXTRA="-DK60_XNULOG -DK60_NO_PHYSMAP_EXT" PAYLOAD="$PAYLOAD" \
    ./build.sh 2>&1 | grep -E "error|appended payload" || true
cp out/shim-boot.img "$OUT"

ls -l "$OUT"
echo
echo "boot:  fastboot boot $OUT"
echo "then:  ./tools/k60-log-pull.sh   (from TWRP)"
echo
# 标记表只放真实出现在日志里的字符串（"panel buffer" 只存在于 0170 的注释里，
# 以前那一条永远是 MISS，等于没检查）。
echo "sanity: the kernel must contain every marker below"
for s in "ufs busy, retry next interval" "panic flush, total" "ABORT %s[%d] esr"; do
    if grep -qaF "$s" "$KERNEL"; then echo "  ok   $s"; else echo "  MISS $s"; fi
done
echo "info: GPU 路线才会有的标记（内核没带 Adreno 代码时缺席属正常）"
for s in "SWEET ADRENO: probe done" "SWEET ADRENO: GPUCC up" "SWEET FB: swap"; do
    if grep -qaF "$s" "$KERNEL"; then echo "  have $s"; else echo "  none $s"; fi
done
