#!/usr/bin/env python3
"""把 GMU 固件转成内核可 include 的 C 数组（M3 方案 A 的脚手架）。

    tools/mk-gmu-fw.py gpu-fw/gmu_gen70000.bin > SweetAdrenoGmuFw.h

只生成头文件，不接进 iokit/conf/files.arm64：等「固件怎么进内核」定了再用。
生成时会回读校验一次（转出来的字节必须与源文件逐字节相同）。
"""
import sys

def main(path):
    data = open(path, 'rb').read()
    if len(data) < 0x100:
        sys.exit('固件太小，不像 GMU 镜像')
    lines = [
        '/* 自动生成，勿手改：python3 tools/mk-gmu-fw.py %s */' % path,
        '/* GMU gen7 (A730 / SM8475) 固件，%d 字节 */' % len(data),
        '#ifndef SWEET_ADRENO_GMU_FW_H',
        '#define SWEET_ADRENO_GMU_FW_H',
        '',
        'static const unsigned char sweet_gmu_fw[] = {',
    ]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append('\t' + ','.join('0x%02x' % b for b in chunk) + ',')
    lines += [
        '};',
        'static const unsigned int sweet_gmu_fw_len = %d;' % len(data),
        '',
        '#endif',
    ]
    text = '\n'.join(lines) + '\n'
    # 回读校验：从生成的文本里再解析出字节，和源文件比
    hexes = [int(t, 16) for t in text.replace('\n', '').replace('\t', '')
             .split('{', 1)[1].split('}', 1)[0].replace(',', ' ').split()]
    if bytes(hexes) != data:
        sys.exit('回读校验失败：生成的数组与源文件不一致')
    if 'sweet_gmu_fw_len = %d' % len(data) not in text:
        sys.exit('长度字段不对')
    sys.stdout.write(text)
    print('回读校验通过：%d 字节/%d 个字' % (len(data), len(hexes)), file=sys.stderr)

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'gpu-fw/gmu_gen70000.bin')
