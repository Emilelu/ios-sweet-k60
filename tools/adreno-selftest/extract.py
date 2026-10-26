#!/usr/bin/env python3
"""按序重建 SweetAdreno.cpp（以及它 include 的同目录头文件）。

不手工抠 "+" 行：从空树开始，把提到该文件的补丁按序号逐个交给 patch(1)，
这样"新建文件"和"改文件"都由 patch 自己处理，多文件补丁也不会串味。

用法： extract.py <repo> <tree> <tag>
"""
import glob
import os
import subprocess
import sys
import tempfile

repo, tree, tag = sys.argv[1], sys.argv[2], sys.argv[3]
target = 'iokit/Kernel/arm/SweetAdreno.cpp'
dest = os.path.join(tree, target)
applied = []

os.makedirs(os.path.dirname(dest), exist_ok=True)
for path in sorted(glob.glob(os.path.join(repo, 'xnu/patches/*.patch'))):
    name = os.path.basename(path)
    text = open(path, encoding='utf-8', errors='replace').read()
    if target not in text:
        continue
    # 一个补丁可能同时改 files.arm64 等别的文件；那棵树里没有它们，只把
    # 我们要的文件那一段喂给 patch，避免交互式提问。
    lines = text.split('\n')
    sections, i = [], 0
    while i < len(lines):
        if (lines[i].startswith('--- ') and i + 1 < len(lines)
                and lines[i + 1].startswith('+++ ')):
            body = [lines[i], lines[i + 1]]
            i += 2
            while i < len(lines) and not (
                    lines[i].startswith('--- ') and i + 1 < len(lines)
                    and lines[i + 1].startswith('+++ ')):
                body.append(lines[i])
                i += 1
            sections.append((body[1][4:].strip(), body))
        else:
            i += 1
    wanted = [b for p, b in sections if p.endswith(target)
              or p.endswith('SweetAdrenoGmuFw.h')]
    if not wanted:
        sys.exit('%s 里没找到 %s 的段落' % (name, target))
    for body in wanted:
        with tempfile.NamedTemporaryFile('w', suffix='.patch', delete=False) as tf:
            tf.write('\n'.join(body) + '\n')
            tmp = tf.name
        with open(tmp, 'rb') as fh:
            r = subprocess.run(['patch', '-p1', '-s', '-d', tree], stdin=fh,
                               capture_output=True)
        os.unlink(tmp)
        if r.returncode != 0:
            sys.exit('应用 %s 失败: %s' % (name, r.stderr.decode()))
    applied.append(name)
    if name.startswith(tag):
        break

print('[1/3] 重建：' + ', '.join(applied))
print('      %s 共 %d 行' % (target, len(open(dest).readlines())))
