# k60-touch harness

Host-side smoke test for the K60 Goodix driver (`bsd/dev/k60_touch*.c`, patches
0240-0252). It is not part of the xnu build; nothing in `xnu/patches` or the CI
workflow refers to it.

`build2.sh` takes the driver straight out of the patch chain (so it compiles the
same sources that go to the phone), drops the arm64 barrier instruction the host
compiler cannot assemble, maps every MMIO window to a fake page, and runs the
driver against:

- a fake GENI engine (command, TX/RX FIFO, DONE, length registers, RCG update
  handshake, and a refusal mode that never completes),
- fake TLMM pin pages (the bit patterns are checked, not just written),
- a fake RSC command slot, counting every attempt to map the block,
- a fake `/k60-touch` ADT node,
- a fake Goodix chip that answers both the Berlin and the GT9xx framings.

Scenarios: `default_stage disabled stage0 cmd_db_missing clocks_off clocks_on
geni_timeout berlin gt9xx no_chip dt_node dt_missing rpmhoff rscprobe_refused
id_readback`.

Run it with `bash build2.sh` (needs a host `cc`, `git` and `python3`). Exit
status 0 means every check passed. `HARNESS_ECHO=1 bash build2.sh berlin` also
prints the captured kernel log.

The checks that matter for the current stage (chip ID readback on hardware):

| Scenario | Asserts |
|---|---|
| `rpmhoff` | with `k60rpmhoff=1` the RSC block is never mapped and never triggered, yet the driver still reaches the ID read |
| `rscprobe_refused` | `k60rpmhoff=1` + `k60rscprobe=1` is refused instead of executing the known-fatal access |
| `dt_node` / `dt_missing` | the `reg`/gpio/axis values come from `/k60-touch` when it exists, and the compiled defaults are used when it does not |
| `id_readback` | `k60touchid=1` raises the stage limit to 4, prints `K60 TOUCH: ID: ...` plus the raw bytes, and exits without polling |
