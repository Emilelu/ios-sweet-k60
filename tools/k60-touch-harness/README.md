# k60-touch harness

Host-side smoke test for `bsd/dev/k60_touch_spi.c` (patches 0219-0223). It is
not part of the xnu build: nothing in `xnu/patches` or the CI workflow refers
to it.

What it does: takes the source out of the patch chain, drops the three arm64
barrier instructions (the only thing a host x86 compiler cannot assemble),
maps every MMIO window to a zeroed memory page, and runs the five stage
functions against that fake hardware. The fake is plain RAM, so it checks code
paths, not register semantics:

- the pin programming bit patterns (function, drive, output enable, pull),
- all five clock bits after the read-modify-write,
- the firmware/protocol decode and the refusal to continue on a non-SPI SE,
- the four-byte FIFO packing round trip, the loopback compare, and that a
  transfer whose DONE bit never arrives still returns instead of hanging,
- both product-id framings,
- the boot args: `k60touchoff=1` must not start a thread, the default stage
  limit must leave GCC and the SE untouched.

Run it with `bash build.sh` (needs a host `cc` and `patch`). Exit status 0
means every check passed.
