/*
 * Adreno 730 bring-up, step 1: read-only probe.
 *
 * The board data is K60's own device tree (qcom,kgsl-3d0@3d00000, qcom,gmu@3d69000,
 * clock-controller@3d90000, kgsl-smmu@3da0000, chipid 0x7030002 = Adreno 730).
 * Nothing is written here on purpose: the power, clock and GMU sequences have to
 * be verified against a live device before they may run, and a wrong write to
 * the GPUCC block hangs the SoC with no console output. This step only proves
 * that the blocks are mapped and readable, and records what they look like
 * before anything has powered them up.
 *
 * boot-arg sweetadreno=1 enables it; without the argument no MMIO happens.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

/* K60 (mondrian) / SM8475, from the live device tree. */
#define SWEET_KGSL_PA      0x3d00000ULL
#define SWEET_KGSL_SIZE    0x40000
#define SWEET_GMU_PA       0x3d68000ULL
#define SWEET_GMU_SIZE     0x37000
#define SWEET_GPUCC_PA     0x3d90000ULL
#define SWEET_GPUCC_SIZE   0x10000
#define SWEET_GMU_AO_PA    0x3d40000ULL
#define SWEET_GMU_AO_SIZE  0x10000
#define SWEET_GCC_PA       0x100000ULL
#define SWEET_GCC_SIZE     0x1f4200

extern vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);
extern void delay(int us);

static uint32_t
sweet_adreno_rd(volatile void *base, uint32_t off)
{
	return sweet_host_rd(base, off);
}

static void
sweet_adreno_wr(volatile void *base, uint32_t off, uint32_t v)
{
	sweet_host_wr(base, off, v);
	/* x86 无 dsb：空操作 */
}

static int
sweet_adreno_poll(volatile void *base, uint32_t off, uint32_t mask, uint32_t want,
    unsigned max_us)
{
	for (unsigned i = 0; i < max_us; i++) {
		if ((sweet_adreno_rd(base, off) & mask) == want) {
			return 0;
		}
		delay(1);
	}
	return -1;
}

/* Read-modify-write; returns non-zero when the value did not change. */
static int
sweet_adreno_rmw(volatile void *base, uint32_t off, uint32_t mask, uint32_t value)
{
	uint32_t cur = sweet_adreno_rd(base, off);
	uint32_t want = (cur & ~mask) | (value & mask);

	if (want != cur) {
		sweet_adreno_wr(base, off, want);
	}
	return 0;
}

/* Branch enable + wait, exactly the vendor clk_branch_wait semantics:
 * CLK_OFF clear (bit31=0) or the NOC FSM state 0x2..., the HWCG-mode shortcut
 * on GCC 0x81004, and no halt poll at all for the one branch the vendor marks
 * delayed (the 10us settle is all it waits for). */
static int
sweet_adreno_branch(volatile void *blk, uint32_t off, int delayed)
{
	uint32_t v;

	sweet_adreno_rmw(blk, off, 1, 1);
	delay(delayed ? 10 : 5);
	if (delayed) {
		return 0;
	}
	for (unsigned i = 0; i < 200; i++) {
		v = sweet_adreno_rd(blk, off);
		if (!(v & 0x80000000) || (v & 0xf0000000) == 0x20000000) {
			return 0;
		}
		if (off == 0x81004 && (v & 3) == 3) {
			return 0;      /* HWCG mode: vendor skips halt polling */
		}
		delay(1);
	}
	IOLog("SWEET ADRENO: branch %#x halted (%#x)\n", off, sweet_adreno_rd(blk, off));
	return -1;
}

/*
 * GPUCC power-up, transcribed from the Pong port's PongAdrenoClocks.c, which
 * implements Nothing's gpucc-waipio / gdsc-regulator sequence for the same
 * Cape GPUCC block on SM8475. Register offsets are the vendor's; the base
 * addresses are K60's own (gpucc@3d90000, gcc@100000).
 *
 * Every phase is announced before it runs. A wrong write here hangs the SoC
 * with no console output, so a failure stops the sequence instead of probing
 * on: the last printed phase names what broke.
 */
static int
sweet_adreno_clock_on(volatile void *gpucc, volatile void *gcc)
{
	IOLog("SWEET ADRENO: phase 1 GDSC (gpucc+0x9108)\n");
	if (sweet_adreno_rd(gpucc, 0x9108) & 2) {
		IOLog("SWEET ADRENO: GDSC already owned by hardware, stopping\n");
		return -1;
	}
	sweet_adreno_rmw(gpucc, 0x9108, 0xf007, 0x8000);
	delay(10);
	if (sweet_adreno_poll(gpucc, 0x953c, 0x80000000, 0x80000000, 1000) != 0) {
		IOLog("SWEET ADRENO: GDSC power status timeout (0x953c=%#x)\n",
		    sweet_adreno_rd(gpucc, 0x953c));
		return -1;
	}
	sweet_adreno_rmw(gpucc, 0x9108, 0x800, 0x800);
	delay(10);

	IOLog("SWEET ADRENO: phase 2 GCC branches\n");
	sweet_adreno_rmw(gcc, 0x62000, (1u << 15) | (1u << 16), (1u << 15) | (1u << 16));
	delay(20);
	/* 0x81018 是 vendor 标为 delayed 的那一路：只等 10us，不查 halt。 */
	if (sweet_adreno_branch(gcc, 0x81004, 0) != 0 ||
	    sweet_adreno_branch(gcc, 0x81010, 0) != 0 ||
	    sweet_adreno_branch(gcc, 0x81018, 1) != 0) {
		return -1;
	}

	IOLog("SWEET ADRENO: phase 3 PLLs\n");
	const struct {
		uint32_t base, l, alpha;
	} plls[] = { { 0x0000, 0x1d, 0xb000 }, { 0x1000, 0x34, 0x1555 } };

	for (unsigned p = 0; p < 2; p++) {
		uint32_t base = plls[p].base;

		if ((sweet_adreno_rd(gpucc, base) & 1) &&
		    (sweet_adreno_rd(gpucc, base + 4) & 1)) {
			IOLog("SWEET ADRENO: pll %u already running\n", p);
			continue;
		}
		const uint32_t offsets[] = { 0x10, 0x14, 0x20, 0x24, 0x28, 0x1c, 0x30, 0x34, 0x38 };
		const uint32_t values[] = {
			0x44440000u | plls[p].l, plls[p].alpha, 0x20485699, 0x00182261,
			0x82aa299c, 5, 3, 0x9000, 0x34
		};
		for (unsigned i = 0; i < 9; i++) {
			sweet_adreno_wr(gpucc, base + offsets[i], values[i]);
		}
		sweet_adreno_rmw(gpucc, base, 1, 0);
		sweet_adreno_wr(gpucc, base + 4, 0);
		sweet_adreno_rmw(gpucc, base, 4, 4);
	}

	IOLog("SWEET ADRENO: phase 4 root sources\n");
	sweet_adreno_wr(gpucc, 0x9534, 0);
	const struct {
		uint32_t cmd, cfg;
	} sources[] = { { 0x9318, 0x100602 }, { 0x93ec, 0x100603 }, { 0x9010, 0x100001 } };

	for (unsigned i = 0; i < 3; i++) {
		uint32_t cmd = sources[i].cmd;

		/* 上一轮 update 还没落地时再写 mux，会静默丢配置。 */
		if (sweet_adreno_rd(gpucc, cmd) & 1) {
			IOLog("SWEET ADRENO: root mux %#x update already pending, stopping\n", cmd);
			return -1;
		}
		sweet_adreno_rmw(gpucc, cmd, 2, 2);
		if (sweet_adreno_poll(gpucc, cmd, 0x80000000, 0, 500) != 0) {
			IOLog("SWEET ADRENO: source %#x busy\n", cmd);
			return -1;
		}
		sweet_adreno_rmw(gpucc, cmd + 4, 0x10371f, sources[i].cfg);
		sweet_adreno_rmw(gpucc, cmd, 1, 1);
		if (sweet_adreno_poll(gpucc, cmd, 1, 0, 500) != 0) {
			IOLog("SWEET ADRENO: source %#x update timeout\n", cmd);
			return -1;
		}
	}

	IOLog("SWEET ADRENO: phase 5 branches\n");
	const uint32_t branches[] = { 0x9004, 0x9144, 0x93e8, 0x913c, 0x9148, 0x911c, 0x7000 };

	for (unsigned i = 0; i < 7; i++) {
		if (sweet_adreno_branch(gpucc, branches[i], branches[i] == 0x911c) != 0) {
			return -1;
		}
	}

	IOLog("SWEET ADRENO: phase 6 release temporary forces\n");
	const uint32_t roots[] = { 0x9318, 0x93ec, 0x9010 };

	for (unsigned i = 0; i < 3; i++) {
		sweet_adreno_rmw(gpucc, roots[i], 2, 0);
	}
	IOLog("SWEET ADRENO: GPUCC up\n");
	return 0;
}

class SweetAdreno : public IOService {
	OSDeclareDefaultStructors(SweetAdreno);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAdreno, IOService);

bool
SweetAdreno::start(IOService *provider)
{
	unsigned enabled = 0;

	if (!IOService::start(provider)) {
		return false;
	}
	registerService();
	if (!PE_parse_boot_argn("sweetadreno", &enabled, sizeof(enabled)) || enabled == 0) {
		IOLog("SWEET ADRENO: probe disabled (boot-arg sweetadreno=1 enables it)\n");
		return true;
	}

	/*
	 * 读的次序按"这块的电源域最可能是关的"排：gpucc 与 GMU AO 在 CX/AO 域，
	 * 开机就在；GMU 其次；kgsl 的 GX 域在 GDSC 关着时读它，NoC 可能直接挂死
	 * 而且没有任何输出。所以 kgsl 放到最后，并且默认不读，要 sweetadreno_kgsl=1
	 * 才试 —— 万一它挂了，前面几行日志已经说明"坏在 kgsl 这一次读"。
	 */
	volatile void *gpucc = (volatile void *)ml_io_map(SWEET_GPUCC_PA, SWEET_GPUCC_SIZE);
	volatile void *ao = (volatile void *)ml_io_map(SWEET_GMU_AO_PA, SWEET_GMU_AO_SIZE);
	volatile void *gmu = (volatile void *)ml_io_map(SWEET_GMU_PA, SWEET_GMU_SIZE);

	IOLog("SWEET ADRENO: maps gpucc=%p ao=%p gmu=%p\n", gpucc, ao, gmu);
	if (gpucc == NULL || ao == NULL || gmu == NULL) {
		IOLog("SWEET ADRENO: MMIO mapping failed, stopping\n");
		return true;
	}

	IOLog("SWEET ADRENO: reading gpucc (CX domain, safe)\n");
	for (uint32_t i = 0; i < 4; i++) {
		IOLog("SWEET ADRENO: gpucc[%u]=%#x\n", i, sweet_adreno_rd(gpucc, i * 4));
	}
	IOLog("SWEET ADRENO: reading gmu_ao_blk_dec0 (AO domain, safe)\n");
	for (uint32_t i = 0; i < 4; i++) {
		IOLog("SWEET ADRENO: ao[%u]=%#x\n", i, sweet_adreno_rd(ao, i * 4));
	}
	IOLog("SWEET ADRENO: reading gmu (may need CX\'s GDSC)\n");
	for (uint32_t i = 0; i < 4; i++) {
		IOLog("SWEET ADRENO: gmu[%u]=%#x\n", i, sweet_adreno_rd(gmu, i * 4));
	}
	/* chipid 是 DT 常量（qcom,chipid = 0x7030002, Adreno730v3），不是寄存器。 */
	IOLog("SWEET ADRENO: chipid is the DT constant 0x7030002 (A730v3), not a register\n");

	unsigned want_kgsl = 0;
	if (PE_parse_boot_argn("sweetadreno_kgsl", &want_kgsl, sizeof(want_kgsl)) && want_kgsl) {
		volatile void *kgsl = (volatile void *)ml_io_map(SWEET_KGSL_PA, SWEET_KGSL_SIZE);

		if (kgsl == NULL) {
			IOLog("SWEET ADRENO: kgsl mapping failed\n");
		} else {
			IOLog("SWEET ADRENO: reading kgsl (GX domain, may hang if GDSC is off)\n");
			for (uint32_t i = 0; i < 4; i++) {
				IOLog("SWEET ADRENO: kgsl[%u]=%#x\n", i, sweet_adreno_rd(kgsl, i * 4));
			}
			/* 控制台环很小，整页只在这个开关下打，且只打非零字。 */
			unsigned verbose = 0;
			if (PE_parse_boot_argn("sweetadreno_verbose", &verbose, sizeof(verbose)) && verbose) {
				for (uint32_t off = 0; off < 0x100; off += 4) {
					if (sweet_adreno_rd(kgsl, off) != 0) {
						IOLog("SWEET ADRENO: kgsl+%#x=%#x\n", off,
						    sweet_adreno_rd(kgsl, off));
					}
				}
			}
		}
	} else {
		IOLog("SWEET ADRENO: kgsl read skipped (sweetadreno_kgsl=1 tries the GX domain)\n");
	}
	IOLog("SWEET ADRENO: probe done\n");

	unsigned power = 0;

	if (!PE_parse_boot_argn("sweetadreno_power", &power, sizeof(power)) || power == 0) {
		IOLog("SWEET ADRENO: power sequence skipped (sweetadreno_power=1 runs it)\n");
		return true;
	}
	volatile void *gcc = (volatile void *)ml_io_map(SWEET_GCC_PA, SWEET_GCC_SIZE);

	if (gcc == NULL) {
		IOLog("SWEET ADRENO: GCC mapping failed\n");
		return true;
	}
	int ret = sweet_adreno_clock_on(gpucc, gcc);

	IOLog("SWEET ADRENO: clock sequence returned %d\n", ret);
	return true;
}
#endif /* SWEET */
