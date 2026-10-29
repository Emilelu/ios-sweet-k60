# k60-goodix-touch — Goodix on QUPv3 SE4 (mondrian / SM8475)

The digitizer of the Redmi K60 is a Goodix part behind the same GENI SPI engine
that the Nothing Phone 2 (pong, also SM8475) uses. The kernel side is the
`bsd/dev/k60_touch*.c` set from `xnu/patches/0240-0252`; this directory holds
the board-side facts: the DTS node, the boot arguments, and the check that the
two copies of the wiring cannot drift apart.

Goal of this stage: **read the chip ID back**, not full coordinate reporting.
That is one boot, one grep, and a concrete register value to quote.

```
drivers/k60-goodix-touch/
├── k60-goodix-touch.dtsi   wiring of record (every value sourced)
├── board-touch.conf        boot args for the ID read (source before building)
├── check-wiring.py         dts <-> tools/mkadt.py consistency check
└── README.md               this file
```

The node the kernel actually reads is `/k60-touch` in the ADT, emitted by
`tools/mkadt.py` (`TOUCH_*` constants). `check-wiring.py` compares the two and
fails on any difference:

```sh
python3 drivers/k60-goodix-touch/check-wiring.py          # 12 values agree
python3 drivers/k60-goodix-touch/check-wiring.py -v       # print the table
```

## 1. Hardware

| Item | Value | Source |
|---|---|---|
| SE | QUPv3 SE4, `0x00990000` + `0x4000` | live FDT `spi@990000` |
| SE IRQ | GIC SPI **605** (`<0 0x25d 4>`) — never claimed, the driver polls | live FDT |
| SPI | mode 0, 8 bit, CS0, 15 MHz ceiling (driver starts at 4 MHz) | FDT `spi-max-frequency = 0xe4e1c0` |
| SPI pins | GPIO16 clk, 17 mosi, 18 miso, 19 cs | live FDT pinctrl |
| reset | GPIO20, active low | FDT `goodix,reset-gpio <&tlmm 0x14 0>` |
| irq | GPIO21, open drain → must be pulled **up** before reading a level | FDT + 0245 |
| avdd | GPIO71, active high; no second PMIC rail on K60 | FDT |
| iovdd | pm8350c LDO C2 (`ldoc2`, `rpmh-regulator-ldoc2`), 1.8 V, mode 7 | live FDT regulator |
| axes | 14400 × 32000 (10 × the 1440×3200 panel, 0.1 mm units) | FDT `goodix,panel-max-x/y` |
| firmware / config | `goodix_firmware_m11a` / `goodix_cfg_group_m11a` | FDT (names only; not loaded yet) |
| GCC | `0x00100000` + `0x70000`; votes at `+0x62008` bits 6/7/8/9/14, SE4 RCG at `+0x274e4` | clkref `gcc-sm8450.c`, live FDT clocks `<&gcc 94 134 135>` |
| TLMM | `0x0f100000`, one `0x1000` page per pin (the base tree's `0x0f000000` wedges the interconnect) | XTOM measurement |

## 2. The one bypass this board has

RSC at `0x17a00000` is unreachable on the measured unit: the first access takes
the whole machine down with no panic (0248's read-only probe produced that
conclusion, and nothing since has contradicted it). The touch IOVDD vote lives
in that block, so it cannot run.

`k60rpmhoff=1` therefore makes the driver skip the vote and never map RSC at
all. The reasoning is falsifiable: TPVDD/IOVDD is left on by the bootloader, so
if the chip answers the ID read, the assumption held. If it does not answer,
IOVDD is the first thing to suspect, not the SPI path.

`k60rpmhoff=1` also refuses `k60rscprobe=1` (0251): that combination is the one
known way to kill the unit, and it must not depend on argument order.

## 3. Read the ID

```sh
cd ~/hackios/ios-sweet            # the checkout tools/k60-build-boot.sh uses
. <repo>/drivers/k60-goodix-touch/board-touch.conf
tools/k60-build-boot.sh <kernel> artifacts/xnu-touch-id.img
# "cmdline extra: k60touchstage=4 k60rpmhoff=1 k60touchdelay=8 k60touchhz=4000000 k60touchid=1"
fastboot boot artifacts/xnu-touch-id.img
```

Then pull the log (`tools/k60-log-pull.sh` from TWRP) and grep:

```sh
grep 'K60 TOUCH:' k60.log
```

### What a pass looks like

```
K60 TOUCH: dt: se 0x990000+0x4000 gcc 0x100000+0x70000 tlmm 0xf100000/0x1000 gpio reset 20 irq 21 avdd 71 spi 15000000 Hz axes 14400 x 32000
K60 TOUCH: init: stage 4 (...), delay 8 s, spi 4000000 Hz, flush 1, id only 1
K60 TOUCH: powered: avdd=1 reset=1 irq level 0
K60 TOUCH: iovdd vote skipped (k60rpmhoff=1), RSC not mapped
K60 TOUCH: gcc vote 0x0 -> 0x43c0, bits 0x43c0            <- 5 bits 6/7/8/9/14
K60 TOUCH: gcc halt m/s/se 0x0/0x0/0x0
K60 TOUCH: clocks ready: vote 0x43c0 halt 0x0/0x0/0x0 source 19200000 Hz
K60 TOUCH: se4 fw 0x102 version 2 protocol 1 (SPI=1)      <- SE firmware really is SPI
K60 TOUCH: se4 spi cfg word 0x4 mclk 0x51 (source 19200000 Hz / 5)
K60 TOUCH: se4 status 0x0 fifo 32 words
K60 TOUCH: ID: abi=berlin chip="911" firmware="..." version=01.00.00.00 sensor=10 report=0x.... command=0x.... axes=14400x32000
K60 TOUCH: ID: raw 0x10014 = 39 31 31 00 ...
```

The two judgement lines are `se4 fw 0x102 ... protocol 1 (SPI=1)` (the SE was
left in SPI mode by the bootloader) and the `K60 TOUCH: ID:` line.

### Concrete register values behind those lines

| Where | Register | Expected | Meaning |
|---|---|---|---|
| SE4 +0x068 | `FW_REVISION_RO` | `0x00000102` | version 2, protocol **1 = SPI**; anything else means the SE is not in SPI mode and the driver stops before touching it |
| SE4 +0x0e24 | `HW_PARAM_0[21:16]` | 32 (or 63) | FIFO depth in 32-bit words; 0 = no FIFO = do not continue |
| SE4 +0x048 | `SER_M_CLK_CFG` | `0x51` at 4 MHz with the XO parent | `(divider 5 << 4) | enable`; the divider is derived from the RCG parent, never programmed |
| GCC +0x62008 | branch vote | reads back `0x43c0` | bits 14 (SE4), 9/8 (core, core2x), 7/6 (s/m ahb) |
| Goodix 0x10014 | fw version block | 28 bytes, last 2 = 16-bit sum | `chip` at +0 (ASCII, e.g. `911`), `firmware` at +10 |
| Goodix 0x10070 | IC info | u16 length, then five tables | +44 (of the misc block) = touch report address, +52/+54 = real axes |
| Goodix 0x8140 | GT9xx product id | 4 ASCII bytes | only read when the Berlin framing does not answer |

### Which ABI answered

| Log | ABI | Next |
|---|---|---|
| `ID: abi=berlin chip="911"` | Berlin (GT9916 class, the pong ABI) | expected on m11a |
| `ID: abi=gt9xx product="9111"` | GT9xx (0x8140 framing) | older part; the poll path uses 0x814e/0x8150 |
| `ID: abi=none` | neither framing answered | power/SPI first: IRQ level after reset, `se4 status`, MISO continuity; do **not** retry RSC |

## 4. Host-side check (no phone)

`tools/k60-touch-harness/build2.sh` compiles the real driver sources against
stub kernel headers and runs them against a fake GENI engine, TLMM pages, an
RSC slot and a Goodix chip:

```sh
bash tools/k60-touch-harness/build2.sh          # 15 scenarios
bash tools/k60-touch-harness/build2.sh berlin   # one scenario
```

The `rpmhoff`, `rscprobe_refused`, `dt_node`/`dt_missing` and `id_readback`
scenarios are the ones that guard this stage: they assert that RSC is never
mapped under `k60rpmhoff=1`, that the ADT node is consumed when present and the
compiled defaults are used when absent, and that `k60touchid=1` prints the ID
line and stops.

## 5. Next: coordinates

ID read is stage 4. Stage 5 is the poll loop that already calls
`sweet_hid_touch()`:

1. `k60touchstage=5` (without `k60touchid`) — the driver reads the report
   block, decodes contacts and publishes them through `SweetHIDTouch`.
   Berlin: report header at the IC-info address, 8-byte contacts, header and
   contact block each carry their own 16-bit sum.
2. Verify one finger: `K60 TOUCH: touch finger 0 at <raw x>, <raw y> -> <px>, <px>`.
   Raw/10 = panel pixel; `k60touchflip=1|2` mirrors if the panel is mounted the
   other way round.
3. Then check the lift path (`count = 0` in the next report), then two fingers
   (IDs must not swap), then edges (clamping is in `k60_to_pixel`).
4. Only after that should `SweetHIDTouch`'s logical scaling be touched: the
   canvas is 1440×3200, so panel pixels go in unchanged.

Open items for the coordinate stage: confirm the Berlin report stride against
the real IC info (the driver assumes 8-byte contacts), and confirm that axis
direction matches the panel orientation before enabling the HID path by default.
