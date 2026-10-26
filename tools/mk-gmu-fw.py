#!/usr/bin/env python3
"""把 GMU 固件转成内核可 include 的 C 数组 + 段表（M3 方案 A）。

    tools/mk-gmu-fw.py gpu-fw/gmu_gen70000.bin > SweetAdrenoGmuFw.h

段表来自上游 AdrenoFirmwarePlanGmu 对同一份固件的解析结果（15 个块归并成 4 段），
内核侧因此不必再实现一遍 TOC 解析。region：1=ITCM 2=cache(GPU 内存) 3=DTCM。

生成后会回读：从输出文本里再解析出字节，必须与源文件逐字节一致，段表也必须自洽。
"""
import sys

# {目标地址, 长度, region, 数组内偏移}
SEGS = [
    (0x00000000, 0x1DC, 1, 0xC0),
    (0x00004000, 0xDCAC, 2, 0x2AC),
    (0x10007F98, 0x28, 3, 0xDF68),
    (0x10007FDC, 0x18, 3, 0xDFA0),
]


def build_text(path, data):
    lines = [
        '/* 自动生成，勿手改：python3 tools/mk-gmu-fw.py %s */' % path,
        '/* GMU gen7 (A730 / SM8475) 固件，%d 字节 */' % len(data),
        '#ifndef SWEET_ADRENO_GMU_FW_H',
        '#define SWEET_ADRENO_GMU_FW_H',
        '',
        'static const unsigned char sweet_gmu_fw[] = {',
    ]
    for i in range(0, len(data), 12):
        lines.append('\t' + ','.join('0x%02x' % b for b in data[i:i + 12]) + ',')
    lines += [
        '};',
        'static const unsigned int sweet_gmu_fw_len = %d;' % len(data),
        '',
        '/* {目标地址, 长度, 目标(1=ITCM 2=cache 3=DTCM), 数组内偏移} */',
        'static const struct { unsigned int addr, bytes, region, off; } sweet_gmu_fw_seg[] = {',
    ]
    for a, n, r, off in SEGS:
        lines.append('\t{ 0x%08x, 0x%x, %d, 0x%x },' % (a, n, r, off))
    lines += [
        '};',
        'static const unsigned int sweet_gmu_fw_seg_count = %d;' % len(SEGS),
        '',
        '#endif',
    ]
    return '\n'.join(lines) + '\n'


def verify(text, data):
    body = text.split('{', 1)[1].split('}', 1)[0]
    got = bytes(int(t, 16) for t in body.replace('\n', '').replace('\t', '')
                .replace(',', ' ').split())
    if got != data:
        sys.exit('回读校验失败：生成的数组与源文件不一致')
    if 'sweet_gmu_fw_len = %d' % len(data) not in text:
        sys.exit('长度字段不对')
    last = SEGS[-1]
    if last[3] + last[1] != len(data):
        sys.exit('段表与固件长度不自洽：%#x + %#x != %d'
                 % (last[3], last[1], len(data)))


def main(path):
    data = open(path, 'rb').read()
    if len(data) < 0x100:
        sys.exit('固件太小，不像 GMU 镜像')
    text = build_text(path, data)
    verify(text, data)
    sys.stdout.write(text)
    print('回读校验通过：%d 字节，%d 段，段表自洽' % (len(data), len(SEGS)),
          file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'gpu-fw/gmu_gen70000.bin')
