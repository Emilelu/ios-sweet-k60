#!/usr/bin/env python3
"""把 SweetAdreno.cpp 变成 x86 可跑的仿真版。

只动三件事，寄存器逻辑一行不改：
  1. 两处裸指针访问换成 sweet_host_rd/wr（用来记录与注入寄存器行为）；
  2. dsb sy 屏障换成空操作（x86 没有这条指令）；
  3. 把 ml_io_map/delay 的声明换成 extern "C" 版本——内核里这两个都是 C 符号，
     C++ 文件里不做 extern "C" 会变名字，链接期找不到（CI 第一次挂的原因）。
     放在这里统一注入，0179 这种还没修的版本也能在同一套桩下编译。
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding='utf-8').read()

text = text.replace(
    'return *(volatile uint32_t *)((volatile char *)base + off);',
    'return sweet_host_rd(base, off);')
text = text.replace(
    '*(volatile uint32_t *)((volatile char *)base + off) = v;',
    'sweet_host_wr(base, off, v);')
text = text.replace('__asm__ volatile("dsb sy" ::: "memory");',
                    '/* x86 无 dsb sy：空操作 */')

kept = []
for line in text.split('\n'):
    if line.startswith('extern vm_offset_t ml_io_map'):
        continue
    if line.startswith('extern void delay'):
        continue
    kept.append(line)
text = '\n'.join(kept)

prologue = '''/* 仿真注入：内核里这两个是 C 符号 */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);
extern "C" void delay(int us);

'''
open(dst, 'w', encoding='utf-8').write(prologue + text)
