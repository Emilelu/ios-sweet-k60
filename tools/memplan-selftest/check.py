#!/usr/bin/env python3
"""Assert that every plan's ADT window only ever frees DRAM the AP can decode.

This is the part that is actually checked, not guessed:

  * the notch table extracted from xnu/patches/0184 must cover every carve-out
    that falls inside each plan's window (and the panel buffer specifically);
  * every page the modelled pmap_startup() would put on a free list must sit
    inside an iomem "System RAM" segment -- nothing else is proven reachable;
  * the panic log must be page aligned, inside System RAM and outside every
    notch;
  * the three known-bad windows (the pre-0172 physmap, the old artifacts/k60.adt,
    and P1 without its gunyah notch) must fail those same checks, otherwise the
    checks are vacuous.
"""
import os
import struct
import subprocess
import sys
import tempfile

import memplan as M

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, 'build')
SIM = os.path.join(BUILD, 'sim')
SESSION_SMOKE = None
FAILS = []


def ok(msg):
    print('  ok   %s' % msg)


def bad(msg):
    print('  FAIL %s' % msg)
    FAILS.append(msg)


def check(cond, msg, detail=''):
    if cond:
        ok(msg)
    else:
        bad(msg + (': ' + detail if detail else ''))


def run_sim(start, end, notches, expect_fail=False):
    argv = [SIM, '--start', hex(start), '--end', hex(end)]
    for lo, hi, _ in notches:
        argv += ['--notch', '%#x:%#x' % (lo, hi)]
    p = subprocess.run(argv, capture_output=True, text=True)
    if expect_fail:
        return p
    if p.returncode != 0:
        bad('sim rejected window %#x-%#x: %s' % (start, end, p.stderr.strip()))
        return None
    stat, runs = {}, []
    for ln in p.stdout.splitlines():
        if ln.startswith('STAT '):
            for kv in ln.split()[1:]:
                k, v = kv.split('=')
                stat[k] = int(v, 0)
        elif ln.startswith('RUN '):
            lo, hi = ln.split()[1].split('-')
            runs.append((int(lo, 16), int(hi, 16)))
    return stat, runs


def mkadt_path():
    # The copy the K60 build actually uses (tools/k60-build-boot.sh points at
    # $HOME/hackios/ios-sweet). The repo's tools/mkadt.py predates K60 support.
    p = os.path.join(M.HACKIOS, 'ios-sweet', 'tools', 'mkadt.py')
    return p if os.path.exists(p) else os.path.join(M.REPO, 'tools', 'mkadt.py')


def mkadt(base, adt_mem, out):
    env = dict(os.environ, ADT_PHYS_BASE=hex(base), ADT_MEM_SIZE=hex(adt_mem))
    p = subprocess.run([sys.executable, mkadt_path(), out, '--k60'],
                       env=env, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit('mkadt.py failed: %s' % p.stderr)
    blob = open(out, 'rb').read()
    have = lambda v: struct.pack('>Q', v)[::-1] in blob
    return blob, have


def main():
    phys = M.load_physmap()
    notches = M.parse_patch_notches()
    access = M.accessible_runs(phys)
    blk = {name: (lo, lo + size) for lo, size, name, _ in phys['carveouts']}
    total_ap = sum(hi - lo for lo, hi in access)

    print('[1/5] notch table shape (%s)' %
          os.path.basename(M.patch_path()))
    check(all(lo % M.PAGE == 0 and hi % M.PAGE == 0 for lo, hi, _ in notches),
          'notches are 16 KiB aligned')
    check(all(notches[i][1] <= notches[i + 1][0] for i in range(len(notches) - 1)),
          'notches are sorted and do not overlap')
    splash = blk['splash_region']
    panel = [n for n in notches if n[0] == splash[0]]
    check(panel and panel[0][1] - panel[0][0] >= 1440 * 3200 * 4,
          'the panel notch starts at the DTS splash base and covers a full frame',
          '%s vs splash %s' % (panel, splash))

    print('\n[2/5] every carve-out inside a plan window is notched')
    for plan in M.PLANS:
        if plan.get('experimental'):
            print('  skip %s (experimental, section 10 of MEMORY-PLAN.md)'
                  % plan['name'])
            continue
        acc = M.account(plan, notches)
        required = M.carved_out(phys, acc['start'], acc['end'])
        missing = []
        for lo, size, name, _ in required:
            hi = lo + size
            if name.startswith('splash_region'):
                hi = min(hi, 0xb9200000)          # only the frame is scanned
            if hi <= acc['start'] or lo >= acc['end']:
                continue
            need = max(0, min(hi, acc['end']) - max(lo, acc['start']))
            have = sum(max(0, min(hi, n_hi) - max(lo, n_lo)) for n_lo, n_hi, _ in notches)
            if have < need:
                missing.append('%s@%#x (%d of %d bytes)' % (name, lo, have, need))
        check(not missing, '%s window %#x-%#x covered' %
              (plan['name'], acc['start'], acc['end']), ', '.join(missing))

    print('\n[3/5] free pages are all inside iomem System RAM')
    for plan in M.PLANS:
        if plan.get('experimental'):
            print('  skip %s (experimental, MEMORY-PLAN.md section 10)' % plan['name'])
            continue
        acc = M.account(plan, notches)
        got = run_sim(acc['start'], acc['end'], notches)
        if got is None:
            continue
        stat, runs = got
        outside = [(lo, hi) for lo, hi in runs
                   if M.uncovered_bytes(lo, hi, access) != 0]
        check(not outside, '%s: %d free run(s), %d free pages (%s MiB) all in System RAM' %
              (plan['name'], len(runs), stat['free'], M.mib(stat['free_bytes'])),
              'leaks into %s' % ['%#x-%#x' % r for r in outside][:4])
        check(stat['free_bytes'] == acc['free_bytes'],
              '%s: accounting model and page walk agree (%s MiB)' %
              (plan['name'], M.mib(stat['free_bytes'])))
        check(acc['end'] < 0xe9f00000,
              '%s: window ends below the qtee/XPU wall at 0xe9f00000' % plan['name'])
        check(all(hi <= M.PHYS_TOP_4G for lo, hi in runs),
              '%s: nothing is freed above 4 GiB' % plan['name'])

    print('\n[4/5] the ADT mkadt.py generates for each plan describes that window')
    tmp = tempfile.mkdtemp(prefix='memplan-')
    can_mkadt = 'ADT_PHYS_BASE' in open(mkadt_path()).read()
    if not can_mkadt:
        print('  skip ADT check: %s has no ADT_PHYS_BASE/ADT_MEM_SIZE support '
              '(the K60 build uses ios-sweet/tools/mkadt.py)' % mkadt_path())
    for plan in (M.PLANS if can_mkadt else []):
        acc = M.account(plan, notches)
        if plan.get('experimental'):
            print('  skip %s ADT (experimental, MEMORY-PLAN.md section 10)' % plan['name'])
            continue
        blob, have = mkadt(plan['base'], plan['adt_mem'], os.path.join(tmp, plan['name'] + '.adt'))
        check(have(plan['base']) and have(acc['adt_dram_size']),
              '%s: dram-base %#x + dram-size %#x (%s MiB)' %
              (plan['name'], plan['base'], acc['adt_dram_size'],
               M.mib(acc['adt_dram_size'])))
        pb = acc['panic_base']
        pe = acc['panic_end']
        check(pb % M.PAGE == 0 and M.uncovered_bytes(pb, pe, access) == 0,
              '%s: panic log %#x-%#x is aligned DRAM' % (plan['name'], pb, pe))
        check(not any(lo < pe and hi > pb for lo, hi, _ in notches) and
              not M.carved_out(phys, pb, pe),
              '%s: panic log avoids every notch and carve-out' % plan['name'])

    print('\n[5/6] the page model reproduces a real boot')
    got = run_sim(0x80000000 + M.OLD_ADT_FOOT, 0xb9200000, [])
    check(got and got[0]['free'] == M.OLD_ADT_FREE_PAGES,
          'the 2026-10-25 bad-ADT window walks to %d free pages (device said %d)' %
          (got[0]['free'], M.OLD_ADT_FREE_PAGES))

    print('\n[6/6] negative controls (these must fail the same rules)')
    # (a) pre-0172: memSize was the physmap span, so the VM managed 4 GiB
    acc = M.account(M.PLANS[0], notches)
    got = run_sim(acc['start'], 0x100000000, notches)
    stat, runs = got
    leak = [(lo, hi) for lo, hi in runs if M.uncovered_bytes(lo, hi, access) != 0]
    check(bool(leak) and max(hi for lo, hi in leak) > 0xe9f00000,
          'pre-0172 physmap window frees non-DRAM above 0xe9f00000 (free %s MiB)' %
          M.mib(stat['free_bytes']), '%d leaking runs' % len(leak))
    # (b) the old artifacts/k60.adt: 0x80000000 + 0x39200000
    p = subprocess.run([SIM, '--start', hex(0x82900000), '--end', hex(0xb9200000)],
                       capture_output=True, text=True)
    stat = dict(kv.split('=') for kv in
                [l for l in p.stdout.splitlines() if l.startswith('STAT ')][0].split()[1:])
    runs = [(int(a, 16), int(b, 16)) for a, b in
            [l.split()[1].split('-') for l in p.stdout.splitlines() if l.startswith('RUN ')]]
    leak = [(lo, hi) for lo, hi in runs if M.uncovered_bytes(lo, hi, access) != 0]
    free_mib = int(stat['free']) * M.PAGE / (1 << 20)
    check(leak and free_mib > 850,
          'old artifacts/k60.adt frees %d MiB including hyp/mpss (%d leaking runs)' %
          (free_mib, len(leak)))
    # (c) P1 with the gunyah notch removed
    acc = M.account(M.PLANS[1], notches)
    trimmed = [n for n in notches if n[0] != 0xe0000000]
    stat, runs = run_sim(acc['start'], acc['end'], trimmed)
    leak = [(lo, hi) for lo, hi in runs if M.uncovered_bytes(lo, hi, access) != 0]
    check(bool(leak), 'P1 without the gunyah notch frees the 0xe0000000 block',
          'the notch is load-bearing')

    print('\nsummary: AP-decodable DRAM below 4 GiB (iomem) = %s MiB' % M.mib(total_ap))
    for plan in M.PLANS:
        acc = M.account(plan, notches)
        print('  %-3s dram %#011x+%#010x  span %#011x-%#011x  free %6s MiB  (%s)' %
              (plan['name'], plan['base'], acc['adt_dram_size'], acc['start'],
               acc['end'], M.mib(acc['free_bytes']), plan['patch']))
    p0 = M.account(M.PLANS[0], notches)
    for plan in M.PLANS[1:]:
        acc = M.account(plan, notches)
        print('  %-3s is +%s MiB over P0' % (plan['name'],
              M.mib(acc['free_bytes'] - p0['free_bytes'])))

    if FAILS:
        print('\n%d FAIL' % len(FAILS))
        return 1
    print('\nall checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
