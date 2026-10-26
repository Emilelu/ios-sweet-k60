#!/usr/bin/env python3
"""按序重建 SweetAdreno.cpp：创建补丁取 "+" 行，后续补丁用 patch(1) 叠加。

用法： extract.py <repo> <tree> <tag>
"""
import glob
import os
import subprocess
import sys

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
    if '+++ b/' + target in text and '--- /dev/null' in text:
        out, inside = [], False
        for line in text.split('\n'):
            if line.startswith('+++ b/'):
                inside = line.strip().endswith(target)
                continue
            if inside and line.startswith('+'):
                out.append(line[1:] + '\n')
        open(dest, 'w').writelines(out)
        applied.append(name + '(新建)')
    else:
        with open(path, 'rb') as patch:
            r = subprocess.run(['patch', '-p1', '-s', '-d', tree], stdin=patch,
                               capture_output=True)
        if r.returncode != 0:
            sys.exit('应用 %s 失败: %s' % (name, r.stderr.decode()))
        applied.append(name)
    if name.startswith(tag):
        break

print('[1/3] 重建：' + ', '.join(applied))
print('      %s 共 %d 行' % (target, len(open(dest).readlines())))
