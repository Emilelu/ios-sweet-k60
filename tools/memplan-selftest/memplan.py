#!/usr/bin/env python3
"""K60 (mondrian / SM8475) physical memory map: derive it, tabulate it, account for it.

Two independent sources, both read-only:

  dump/live.dts    the live FDT the ABL hands to Android. Gives the DRAM banks
                   and every fixed no-map carve-out (hyp, smem, modem, cdsp,
                   qtee, ...). This is the *layout*, not the accessibility.
  dump/iomem.txt   /proc/iomem of the same phone running Android. Gives what
                   the AP (Linux, same EL) actually got to use: "System RAM"
                   lines are proven-accessible DRAM, everything else is
                   reserved or simply not decoded.

Neither alone is enough: the DTS calls 0x80000000..0xea000000 "memory" but a
third of it belongs to firmware, and it does not mention the unbacked hole
above 0xea000000 at all. iomem knows what Linux could touch but not *why* a
range is missing.

The derived table is checked in as k60-physmap.json (with the sha256 of both
sources) so the selftest still runs where the dumps are absent.

  memplan.py --emit-json          rewrite k60-physmap.json from the dumps
  memplan.py --verify             re-derive and compare (drift detector)
  memplan.py --table              print the human table
  memplan.py --plans              print the window accounting for every plan
"""
import hashlib
import json
import os
import re
import sys

PAGE = 0x4000                 # arm64 16 KiB granule
SIZEOF_VM_PAGE = 80           # see the note in sim.c
PHYS_TOP_4G = 0x100000000

# topOfKernelData - dram-base: the first byte of the window the VM may use.
# Everything below it is ours (payload segments, ADT, boot_args). The shim
# places it (ios-sweet/shim/loader.c:180-205):
#
#   adt_pa = phys_base + (kernel_end - virt_base)   # segments, then the ADT
#   ba_pa  = align16k(adt_pa + adt_size)            # pad, then boot_args
#   top    = ba_pa + 0x4000                         # one 16 KiB boot_args page
#   foot   = top - phys_base
#
# So it is a property of the kernel image, not of the machine: every rebuild
# that changes the payload's text/data size moves it, and the free-page count
# moves with it 1:1 at 16 KiB granularity / 1 MiB per 64 pages.
#
# For artifacts/xnu-adreno-m1m3-verify.img (payload header, file offset 0x30000)
# the fields are virt_base 0xfffffe0006000000, kernel_end 0xfffffe00084dc000,
# adt_size 0x2774, phys_base 0xb4000000, which gives adt_pa 0xb64dc000,
# ba_pa 0xb64e0000, top 0xb64e4000 and therefore foot 0x24e4000. The previous
# 0x24e0000 stopped one 16 KiB page lower, at ba_pa, and so freed one page too
# many: 41272 instead of 41271, and likewise P1/P2 (46042/62869 -> 46041/62868).
#
# The 2026-10-25 bad-ADT boot (dram 0x80000000 + 0x39200000, "vm_page_bootstrap:
# 55885 free pages") pins its own value to 0x2488000 -- check.py uses that to
# prove this page model reproduces real device output to the page.
KERNEL_FOOT = 0x24e4000
OLD_ADT_FOOT = 0x2488000      # kernel.development.sweet, 2026-10-25 boot
OLD_ADT_FREE_PAGES = 55885   # what that boot printed

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HACKIOS = os.path.dirname(REPO)
DTS = os.path.join(HACKIOS, 'dump', 'live.dts')
IOMEM = os.path.join(HACKIOS, 'dump', 'iomem.txt')
PHYSMAP = os.path.join(HERE, 'k60-physmap.json')
PATCH_GLOB = re.compile(r'^0184-k60-ram-notches\.patch$')


def _cells(text):
    return [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', text)]


def parse_dts(path):
    s = open(path, errors='replace').read()
    out = {'banks': [], 'carveouts': [], 'pools': [], 'ddr_size': None}

    # /memory: reg = <base_hi base_lo size_hi size_lo> * n
    i = s.find('\tmemory {')
    node = s[i:s.find('};', i)]
    c = _cells(re.search(r'reg = <([^;]*)>;', node).group(1))
    assert len(c) % 4 == 0, 'memory node is not 4 cells per bank'
    for k in range(0, len(c), 4):
        out['banks'].append([(c[k] << 32) | c[k + 1], (c[k + 2] << 32) | c[k + 3]])
    m = re.search(r'ddr_size = <(0x[0-9a-fA-F]+)>', node)
    if m:
        out['ddr_size'] = int(m.group(1), 16)

    # /reserved-memory: fixed reg entries only; the reusable pools have no reg
    i = s.find('reserved_memory: reserved-memory')
    blk = s[i:s.find('\t};\n', s.find('splash_region', i))]
    parts = re.split(r'\n\t\t(?:[A-Za-z0-9_]+: )?([A-Za-z0-9_\-@\.]+) \{', blk)
    for k in range(1, len(parts), 2):
        name, body = parts[k], parts[k + 1]
        regm = re.search(r'reg = <([^;]*)>;', body)
        if regm:
            c = _cells(regm.group(1))
            if len(c) == 4:
                base, size = (c[0] << 32) | c[1], (c[2] << 32) | c[3]
                out['carveouts'].append([base, size, name, 'no-map' in body])
            continue
        # dynamic pool: size + alloc-ranges, placed by the OS at runtime
        ms = re.search(r'size = <([^;]*)>;', body)
        if not ms:
            continue
        size = _cells(ms.group(1))[-1]
        lo, hi = 0, 0xffffffff
        mr = re.search(r'alloc-ranges = <([^;]*)>;', body)
        if mr:
            c = _cells(mr.group(1))
            if len(c) == 4:
                lo, hi = (c[0] << 32) | c[1], (c[2] << 32) | c[3] + 1
        out['pools'].append([name, size, lo, hi])

    out['carveouts'].sort(key=lambda r: r[0])
    return out


def parse_iomem(path):
    ram, sub = [], []
    for ln in open(path):
        m = re.match(r'^(\s*)([0-9a-f]+)-([0-9a-f]+) : (.*)$', ln.rstrip('\n'))
        if not m:
            continue
        ind = len(m.group(1))
        lo, hi = int(m.group(2), 16), int(m.group(3), 16) + 1
        what = m.group(4).strip()
        if what == 'System RAM' and ind == 0:
            ram.append([lo, hi])
        elif ind > 0 and lo >= 0x80000000:
            sub.append([lo, hi, what])
    ram.sort()
    return {'system_ram': ram, 'sub_reserved': sub}


def derive():
    dts = parse_dts(DTS)
    iom = parse_iomem(IOMEM)
    return {
        'provenance': {
            'dts': {'path': 'dump/live.dts',
                    'sha256': hashlib.sha256(open(DTS, 'rb').read()).hexdigest()},
            'iomem': {'path': 'dump/iomem.txt',
                      'sha256': hashlib.sha256(open(IOMEM, 'rb').read()).hexdigest()},
            'how': 'tools/memplan-selftest/memplan.py --emit-json',
        },
        'page_size': PAGE,
        'ddr_size_gib': dts['ddr_size'],
        'dts_banks': dts['banks'],
        'carveouts': dts['carveouts'],
        'pools': dts['pools'],
        'iomem_system_ram': iom['system_ram'],
        'iomem_sub_reserved': iom['sub_reserved'],
    }


def load_physmap(verify=True):
    doc = json.load(open(PHYSMAP))
    if verify and os.path.exists(DTS) and os.path.exists(IOMEM):
        fresh = derive()
        for key in ('dts_banks', 'carveouts', 'pools', 'iomem_system_ram',
                    'iomem_sub_reserved', 'ddr_size_gib'):
            if doc[key] != fresh[key]:
                sys.exit('k60-physmap.json is stale: %s differs from the dumps '
                         '(rerun memplan.py --emit-json)' % key)
    return doc


def patch_path():
    pdir = os.path.join(REPO, 'xnu', 'patches')
    hits = [n for n in sorted(os.listdir(pdir)) if PATCH_GLOB.match(n)]
    if not hits:
        sys.exit('no 0184 ram-notch patch in %s' % pdir)
    return os.path.join(pdir, hits[-1])


def parse_patch_notches(path=None):
    """Extract `{ 0xSTART, 0xEND, "why" },` from the kernel notch table."""
    path = path or patch_path()
    out = []
    for ln in open(path):
        if not ln.startswith('+'):
            continue
        m = re.match(r'\+\s*\{\s*(0x[0-9a-fA-F]+)ULL?,\s*(0x[0-9a-fA-F]+)ULL?,'
                     r'\s*"([^"]*)"\s*\},', ln)
        if m:
            out.append([int(m.group(1), 16), int(m.group(2), 16), m.group(3)])
    assert out, 'no notch table found in %s' % path
    return out


# --------------------------------------------------------------------------
# plans: where the payload sits (ADT dram-base) and how far up it manages
# --------------------------------------------------------------------------

PLANS = [
    dict(name='P0', base=0xb4000000, adt_mem=0x2c000000, panic=0x100000,
         title='today: payload at 0xb4000000, stop under the gunyah block',
         patch='0172 only (shipped)'),
    dict(name='P1', base=0xb4000000, adt_mem=0x315f0000, panic=0x100000,
         title='same base, top +74.9 MiB (into 0xe0b00000..0xe54f0000)',
         patch='0184 (notches)'),
    dict(name='P2', base=0xa3000000, adt_mem=0x425f0000, panic=0x100000,
         title='payload moved into the 61.5 MiB hole at 0xa2e00000',
         patch='0184 + ADT window only'),
    # P4 is the "if Android can, why can't we" arithmetic, not a plan: one span
    # from the low hole to the top of the last DRAM bank (42 GiB), with every
    # non-DRAM / firmware range notched. It is only reachable if the banks above
    # 4 GiB decode for the AP at all -- see MEMORY-PLAN.md section 10.
    dict(name='P4', base=0xa3000000, end=0xa7ff00000, panic=0x100000,
         title='EXPERIMENT: whole device in one span (needs >4 GiB to decode)',
         patch='0184 + notch table for 28 GiB of holes',
         experimental=True,
         extra_notches=[
             [0xe55f3000, 0xe5700000, 'trust_ui_vm'],
             [0xe5c00000, 0xea000000, 'linux_reserved+tz_stat+tags+qtee'],
             [0xea000000, 0xf4600000, 'no DRAM'],
             [0xf5000000, 0xffc00000, 'linux reserved'],
             [0x100000000, 0x800000000, 'no DRAM (28 GiB hole)'],
             [0xa69b78000, 0xa7ff00000, 'high-bank reserved (gunyah?)'],
         ]),
]


def account(plan, notches, foot=KERNEL_FOOT):
    base = plan['base']
    all_notches = list(notches) + [list(n) for n in plan.get('extra_notches', [])]
    end = plan['end'] if plan.get('end') else base + plan['adt_mem'] - plan['panic']
    panic_base = end
    start = base + foot
    span = end - start
    span_pages = span // PAGE
    # pmap_startup: mem_sz = pmap_free_pages_span()*PAGE
    #               npages  = mem_sz / (PAGE + sizeof(*vm_pages))
    npages = span // (PAGE + SIZEOF_VM_PAGE)
    array_pages = (npages * SIZEOF_VM_PAGE + PAGE - 1) // PAGE
    first = start + array_pages * PAGE

    def notched(lo, hi):
        lo, hi = max(lo, first), min(hi, end)
        return max(0, (hi - lo) // PAGE)

    notch_pages = sum(notched(lo, hi) for lo, hi, _ in all_notches)
    free_pages = (end - first) // PAGE - notch_pages
    return dict(plan=plan, start=start, end=end, panic_base=panic_base,
                panic_end=panic_base + plan['panic'], span=span,
                span_pages=span_pages, npages=npages, array_pages=array_pages,
                first=first, notch_pages=notch_pages, free_pages=free_pages,
                free_bytes=free_pages * PAGE,
                adt_dram_size=(end - base),
                notches=all_notches)


def free_runs(acc, notches):
    """[lo,hi) byte runs that end up on a free list, from `first` to `end`."""
    runs, cur = [], acc['first']
    for lo, hi, _ in sorted(acc.get('notches', notches)):
        lo, hi = max(lo, acc['first']), min(hi, acc['end'])
        if lo >= hi:
            continue
        if cur < lo:
            runs.append([cur, lo])
        cur = max(cur, hi)
    if cur < acc['end']:
        runs.append([cur, acc['end']])
    return runs


def accessible_runs(phys):
    """iomem System RAM below 4 GiB: DRAM the AP demonstrably decoded."""
    return [(lo, hi) for lo, hi in phys['iomem_system_ram'] if lo < PHYS_TOP_4G]


def uncovered_bytes(lo, hi, runs):
    """bytes of [lo,hi) that no run covers"""
    miss, cur = 0, lo
    for rlo, rhi in runs:
        if rhi <= cur or rlo >= hi:
            continue
        if rlo > cur:
            miss += min(rlo, hi) - cur
        cur = max(cur, rhi)
        if cur >= hi:
            break
    if cur < hi:
        miss += hi - cur
    return miss


def carved_out(phys, lo, hi):
    """carve-outs (base,base+size) that intersect [lo,hi)"""
    return [c for c in phys['carveouts'] if c[0] < hi and c[0] + c[1] > lo]


def mib(n):
    return '%.1f' % (n / (1 << 20))


def print_table(phys):
    print('DRAM banks (dump/live.dts /memory, vendor DTB; banks 3+5 overlap):')
    for base, size in phys['dts_banks']:
        print('  %#012x + %#011x -> %#012x  %9s MiB' %
              (base, size, base + size, mib(size)))
    print('  ddr_size field = %s GiB (physical parts, not what the AP sees)'
          % phys['ddr_size_gib'])

    print('\nFixed no-map carve-outs (dump/live.dts /reserved-memory):')
    tot = 0
    for base, size, name, _ in phys['carveouts']:
        tot += size
        print('  %#012x-%#012x %8s MiB  %s' % (base, base + size, mib(size), name))
    print('  %d carve-outs, %s MiB total' % (len(phys['carveouts']), mib(tot)))

    print('\niomem System RAM (what Android actually used) below 4 GiB:')
    for lo, hi in accessible_runs(phys):
        print('  %#012x-%#012x %9s MiB' % (lo, hi, mib(hi - lo)))

    print('\niomem sub-reservations (software reservations, not hardware):')
    for lo, hi, what in phys['iomem_sub_reserved']:
        print('  %#012x-%#012x %8s MiB  %s' % (lo, hi, mib(hi - lo), what))


def print_plans(phys, notches):
    print('notch table from %s:' % os.path.basename(patch_path()))
    for lo, hi, why in notches:
        print('  %#012x-%#012x %8s MiB  %s' % (lo, hi, mib(hi - lo), why))
    print()
    print('%-4s %-11s %-14s %-23s %-11s %s' %
          ('plan', 'dram-base', 'adt dram-size', 'managed window', 'free', 'free from'))
    for plan in PLANS:
        a = account(plan, notches)
        print('%-4s %#011x %#012x %#011x-%#011x %8s MiB %#011x' %
              (plan['name'], plan['base'], a['adt_dram_size'], a['start'], a['end'],
               mib(a['free_bytes']), a['first']))
    print()
    for plan in PLANS:
        a = account(plan, notches)
        print('%s: %s' % (plan['name'], plan['title']))
        print('     panic %#x..%#x  span %s MiB  array %d pages  notched %d pages  '
              'free %d pages (%s MiB)  needs: %s' %
              (a['panic_base'], a['panic_end'], mib(a['span']), a['array_pages'],
               a['notch_pages'], a['free_pages'], mib(a['free_bytes']), plan['patch']))


def main(argv):
    if '--emit-json' in argv:
        json.dump(derive(), open(PHYSMAP, 'w'), indent=1)
        open(PHYSMAP, 'a').write('\n')
        print('wrote %s' % PHYSMAP)
        return 0
    if '--verify' in argv:
        load_physmap(verify=True)
        print('k60-physmap.json matches dump/live.dts + dump/iomem.txt')
        return 0
    phys = load_physmap(verify=True)
    if '--table' in argv:
        print_table(phys)
    if '--plans' in argv:
        print_plans(phys, parse_patch_notches())
    if not argv:
        print(__doc__)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
