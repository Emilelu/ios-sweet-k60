#!/usr/bin/env python3
"""Fail when the touch wiring in the DTS and in tools/mkadt.py disagree.

drivers/k60-goodix-touch/k60-goodix-touch.dtsi is the wiring of record; the
kernel does not read it.  tools/mkadt.py is what actually reaches the kernel as
the /k60-touch node.  Two copies of the same numbers is one copy too many, so
this script reads both and compares them before a build goes out.

    python3 drivers/k60-goodix-touch/check-wiring.py [--repo DIR]

Exit status 0 means every shared value matches.  --verbose prints the table.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read(path):
    with open(path, encoding='utf-8') as handle:
        return handle.read()


def dts_values(text):
    values = {}
    for name, value in re.findall(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)', text):
        values[name] = int(value, 0)
    # The three control lines: <&tlmm N ...>
    for prop, key in (('goodix,reset-gpio', 'reset'),
                      ('goodix,irq-gpio', 'irq'),
                      ('goodix,avdd-gpio', 'avdd')):
        m = re.search(prop + r'\s*=\s*<&tlmm\s+(\d+)', text)
        if not m:
            raise SystemExit('dts: %s not found' % prop)
        values['gpio_' + key] = int(m.group(1))
    for prop, key in (('goodix,panel-max-x', 'abs_x'),
                      ('goodix,panel-max-y', 'abs_y')):
        m = re.search(prop + r'\s*=\s*<(\d+)>', text)
        if not m:
            raise SystemExit('dts: %s not found' % prop)
        values[key] = int(m.group(1))
    m = re.search(r'spi-max-frequency\s*=\s*<(\d+)>', text)
    if not m:
        raise SystemExit('dts: spi-max-frequency not found')
    values['spi_hz'] = int(m.group(1))
    return values


def mkadt_values(text):
    """Parse the TOUCH_* assignments, including the tuple form
    `TOUCH_SE, TOUCH_SE_SIZE = 0x00990000, 0x4000`."""
    values = {}
    for line in text.splitlines():
        if not line.startswith('TOUCH_'):
            continue
        names, _, rhs = line.partition('=')
        names = [n.strip() for n in names.split(',')]
        rhs = [v.strip() for v in rhs.split(',')]
        if len(names) != len(rhs):
            continue
        for name, value in zip(names, rhs):
            try:
                values[name] = int(value, 0)
            except ValueError:
                pass
    want = ('TOUCH_SE', 'TOUCH_SE_SIZE', 'TOUCH_GCC', 'TOUCH_GCC_WINDOW',
            'TOUCH_TLMM', 'TOUCH_TLMM_STRIDE', 'TOUCH_GPIO_RESET',
            'TOUCH_GPIO_IRQ', 'TOUCH_GPIO_AVDD', 'TOUCH_ABS_X', 'TOUCH_ABS_Y',
            'TOUCH_SPI_HZ')
    missing = [name for name in want if name not in values]
    if missing:
        raise SystemExit('mkadt.py: missing %s' % ', '.join(missing))
    if "'k60-touch'" not in text:
        raise SystemExit("mkadt.py: no 'k60-touch' node")
    return values


PAIRS = (
    ('SE4_BASE', 'TOUCH_SE'),
    ('SE4_SIZE', 'TOUCH_SE_SIZE'),
    ('GCC_BASE', 'TOUCH_GCC'),
    ('GCC_WINDOW', 'TOUCH_GCC_WINDOW'),
    ('TLMM_BASE', 'TOUCH_TLMM'),
    ('TLMM_STRIDE', 'TOUCH_TLMM_STRIDE'),
    ('gpio_reset', 'TOUCH_GPIO_RESET'),
    ('gpio_irq', 'TOUCH_GPIO_IRQ'),
    ('gpio_avdd', 'TOUCH_GPIO_AVDD'),
    ('abs_x', 'TOUCH_ABS_X'),
    ('abs_y', 'TOUCH_ABS_Y'),
    ('spi_hz', 'TOUCH_SPI_HZ'),
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    dts = os.path.join(HERE, 'k60-goodix-touch.dtsi')
    mkadt = os.path.join(args.repo, 'tools', 'mkadt.py')
    d = dts_values(read(dts))
    m = mkadt_values(read(mkadt))

    bad = 0
    for dts_name, adt_name in PAIRS:
        left, right = d[dts_name], m[adt_name]
        ok = left == right
        if not ok:
            bad += 1
        if args.verbose or not ok:
            print('%-12s dts %#-10x mkadt %#-10x %s'
                  % (dts_name, left, right, 'ok' if ok else 'MISMATCH'))
    if bad:
        print('%d value(s) differ: fix the dts and mkadt.py together' % bad)
        return 1
    print('wiring ok: 12 values agree between the dts and mkadt.py')
    return 0


if __name__ == '__main__':
    sys.exit(main())
