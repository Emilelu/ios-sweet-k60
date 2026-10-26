/*
 * SweetAdreno 离线仿真台。
 *
 * 把补丁里的 SweetAdreno.cpp 原样编译进来，用一块假的寄存器文件喂它：
 * ml_io_map() 返回对应块的 word 数组，寄存器行为（CLK_OFF 清位、GDSC 上电
 * 状态位、PLL/mux 更新位）由 hook 模拟，于是 M1 探测与 M2 时钟序列可以在
 * 没有手机的情况下跑完，并逐条检查上游 Pong 实现的语义。
 *
 * 编译：clang++ -std=c++17 -DSWEET -I stubs -I . host.cpp -o selftest
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <algorithm>

typedef unsigned long vm_offset_t;
typedef unsigned long vm_size_t;

/* 仿真寄存器文件：一个块 = 一段真实可读写内存 */
struct Block {
	uint64_t pa;
	uint32_t size;
	const char *name;
	std::vector<uint32_t> w;
	/* hook: 返回写入后应保留的读值；is_write 为 false 时 val 无意义 */
	std::function<uint32_t(uint32_t off, bool is_write, uint32_t val)> hook;
};
static std::vector<Block> g_blocks;
static std::vector<std::string> g_access;      /* 顺序访问记录 */
static std::vector<std::string> g_writes;      /* 只记写 */
static std::map<std::string, unsigned> g_bootargs;
static unsigned long g_delay_us;

static Block *blk(uint64_t pa)
{
	for (auto &b : g_blocks) if (b.pa == pa) return &b;
	return nullptr;
}

static bool g_mapfail;

extern "C" vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size)
{
	Block *b = blk((uint64_t)phys);
	if (g_mapfail && (uint64_t)phys == (uint64_t)0x3d90000) {
		printf("[host] 故意让 gpucc 映射失败\n");
		return 0;
	}
	if (!b) { printf("[host] ml_io_map(%#lx) 没有对应块\n", (unsigned long)phys); return 0; }
	if (size > b->size) { printf("[host] ml_io_map(%#lx, %#lx) 超出块大小 %#x\n",
	    (unsigned long)phys, (unsigned long)size, b->size); return 0; }
	return (vm_offset_t)b->w.data();
}

extern "C" void delay(int us) { g_delay_us += us > 0 ? (unsigned long)us : 0; }

extern "C" int PE_parse_boot_argn(const char *name, void *out, int max)
{
	auto it = g_bootargs.find(name);
	if (it == g_bootargs.end()) return 0;
	memset(out, 0, max);
	unsigned v = it->second;
	memcpy(out, &v, std::min((size_t)max, sizeof(v)));
	return 1;
}

extern "C" void IOLog(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	printf("  log| "); vprintf(fmt, ap); va_end(ap);
}

uint32_t sweet_host_rd(volatile void *base, uint32_t off);
void sweet_host_wr(volatile void *base, uint32_t off, uint32_t val);

#include "SweetAdreno-sim.cpp"

/* ---------- 仿真侧读写入口（由 SweetAdreno-sim.cpp 调用） ---------- */
uint32_t sweet_host_rd(volatile void *base, uint32_t off);
void sweet_host_wr(volatile void *base, uint32_t off, uint32_t val);
static Block *blk_by_base(volatile void *base);

/* ---------- 寄存器行为 ---------- */
static bool is_gpu_branch(uint32_t off)
{
	static const uint32_t l[] = { 0x9004, 0x9144, 0x93e8, 0x913c, 0x9148, 0x911c, 0x7000 };
	for (uint32_t x : l) if (x == off) return true;
	return false;
}
static bool is_source_mux(uint32_t off)
{
	static const uint32_t l[] = { 0x9318, 0x93ec, 0x9010 };
	for (uint32_t x : l) if (x == off) return true;
	return false;
}

static std::string g_scenario = "happy";

static void install_hooks()
{
	Block *g = blk(SWEET_GPUCC_PA), *c = blk(SWEET_GCC_PA);
	/* GDSC：写 0x9108 bit15 之后，0x953c bit31 才置起 */
	g->hook = [g](uint32_t off, bool wr, uint32_t val) -> uint32_t {
		if (wr && off == 0x9108 && (val & 0x8000)) g->w[0x953c / 4] |= 0x80000000u;
		if (wr && off == 0x9108 && (val & 0x800)) g->w[0x953c / 4] |= 0x80000000u;
		if (wr && is_gpu_branch(off) && (val & 1)) {
			if (off == 0x911c && g_scenario == "delayed") return 0xa0000000u; /* CLK_OFF 一直不清 */
			return 0x00000000u;                                             /* 正常清掉 CLK_OFF */
		}
		if (wr && is_source_mux(off) && (val & 1)) {
			uint32_t cur = g->w[off / 4];
			cur &= ~1u;                    /* 硬件自清 update 位 */
			cur &= ~0x80000000u;           /* 不 busy */
			return cur;
		}
		return g->w[off / 4];
	};
	c->hook = [c](uint32_t off, bool wr, uint32_t val) -> uint32_t {
		if (wr && (off == 0x81004 || off == 0x81010 || off == 0x81018) && (val & 1)) {
			/* GCC 0x81004 在 HWCG 模式下 CLK_OFF 保持、且 bits0..1==3，
			 * 上游 clk_branch_wait 视其为成功。 */
			if (off == 0x81004 && g_scenario == "hwcg") return 0xa0000003u;
			if (off == 0x81018) return 0x00000000u;   /* 上游对该支路只看延时 */
			return 0x00000000u;
		}
		return c->w[off / 4];
	};
	/* busy 场景：根 mux 的 update 位残留为 1 */
	if (g_scenario == "busy") g->w[0x9318 / 4] = 1;
	/* prepowered：固件已经打开 GDSC（0x9108 bit1），且七条 branch 都在跑 */
	if (g_scenario == "prepowered") {
		g->w[0x9108 / 4] = 2;
		for (uint32_t off : { 0x9004u, 0x9144u, 0x93e8u, 0x913cu, 0x9148u, 0x911cu, 0x7000u })
			g->w[off / 4] = 0;
	}
	/* stuck：branch 写完 CLK_OFF 也不消（0xa... 不是 NOC 的 0x2...） */
	/* stuck：等 CLK_OFF 期间一切正常，临时 force 一放（phase 6）时钟就垮掉。
	 * 这正是"写完不等于时钟在跑"，用来验证收尾自检真的会失败。 */
	if (g_scenario == "stuck") {
		g->hook = [g](uint32_t off, bool wr, uint32_t val) -> uint32_t {
			if (wr && off == 0x9108 && (val & 0x8000)) g->w[0x953c / 4] |= 0x80000000u;
			if (wr && is_source_mux(off) && !(val & 2)) {
				for (uint32_t b : { 0x9004u, 0x9144u, 0x93e8u, 0x913cu, 0x9148u, 0x911cu, 0x7000u })
					g->w[b / 4] = 0xa0000000u;
				return g->w[off / 4] & ~2u;
			}
			if (wr && is_gpu_branch(off) && (val & 1)) return 0;
			if (wr && is_source_mux(off) && (val & 1)) return g->w[off / 4] & ~1u;
			return g->w[off / 4];
		};
	}
	if (g_scenario == "mapfail") g_mapfail = true;
	/* gmulive：GMU 已跑起来（FW_INIT=0x100、HFI up、不在复位）。
	 * 偏移用 SWEET_GMU_OFF 的算法手算：0x1f800→0x16000 等。 */
	if (g_scenario == "gmulive") {
		Block *m = blk(0x3d68000);
		m->w[0x16000 / 4] = 0;                 /* CM3_SYSRESET */
		m->w[0x16070 / 4] = 0x100;             /* CM3_FW_INIT_RESULT */
		m->w[0x16600 / 4] = 1;                 /* HFI_CTRL_STATUS */
	}
}

static Block *blk_by_base(volatile void *base)
{
	for (auto &b : g_blocks) if ((volatile void *)b.w.data() == base) return &b;
	return nullptr;
}
uint32_t sweet_host_rd(volatile void *base, uint32_t off)
{
	Block *b = blk_by_base(base);
	if (!b || off + 4 > b->size) return 0xffffffffu;
	uint32_t v = b->w[off / 4];
	if (b->hook) v = b->hook(off, false, v);
	g_access.push_back(std::string(b->name) + "+R" + std::to_string(off));
	return v;
}
void sweet_host_wr(volatile void *base, uint32_t off, uint32_t val)
{
	Block *b = blk_by_base(base);
	if (!b || off + 4 > b->size) return;
	b->w[off / 4] = val;
	g_access.push_back(std::string(b->name) + "+W" + std::to_string(off));
	if (b->hook) {
		uint32_t after = b->hook(off, true, val);
		if (after != val) b->w[off / 4] = after;
	}
	g_writes.push_back(std::string(b->name) + "+" + std::to_string(off) + "=" + std::to_string(b->w[off / 4]));
}

int main(int argc, char **argv)
{
	g_scenario = argc > 1 ? argv[1] : "happy";
	std::string bootargs = argc > 2 ? argv[2] : "sweetadreno=1";
	/* 解析 k=v,k=v */
	size_t p = 0;
	while (p < bootargs.size()) {
		size_t q = bootargs.find(',', p);
		std::string kv = bootargs.substr(p, q == std::string::npos ? std::string::npos : q - p);
		size_t e = kv.find('=');
		if (e != std::string::npos) g_bootargs[kv.substr(0, e)] = (unsigned)strtoul(kv.c_str() + e + 1, nullptr, 0);
		if (q == std::string::npos) break;
		p = q + 1;
	}
	struct { uint64_t pa; uint32_t size; const char *name; } blocks[] = {
		{ SWEET_KGSL_PA,   SWEET_KGSL_SIZE,   "kgsl"   },
		{ SWEET_GMU_PA,    SWEET_GMU_SIZE,    "gmu"    },
		{ SWEET_GPUCC_PA,  SWEET_GPUCC_SIZE,  "gpucc"  },
		{ SWEET_GMU_AO_PA, SWEET_GMU_AO_SIZE, "ao"     },
		{ SWEET_GCC_PA,    SWEET_GCC_SIZE,    "gcc"    },
	};
	for (auto &b : blocks) {
		Block nb; nb.pa = b.pa; nb.size = b.size; nb.name = b.name;
		nb.w.assign(b.size / 4, 0);
		g_blocks.push_back(nb);
	}
	install_hooks();

	printf("=== 场景 %s，boot-args \"%s\" ===\n", g_scenario.c_str(), bootargs.c_str());
	SweetAdreno svc;
	svc.start(nullptr);
	printf("=== 延时合计 %lu us，读取 %zu 次，写入 %zu 次 ===\n",
	    g_delay_us, g_access.size(), g_writes.size());
	int bad = 0;
	if (g_bootargs.count("sweetadreno") == 0) {
		int ok = g_access.empty();
		printf("ASSERT no-mmio: %s\n", ok ? "ok" : "FAIL（没开开关却碰了 MMIO）");
		bad |= !ok;
	}
	/* stage>=2 等同于 sweetadreno_power=1，会写寄存器；别把它算成只读路径 */
	unsigned stage = g_bootargs.count("sweetadreno_stage") ? g_bootargs["sweetadreno_stage"] : 0;
	if (((g_bootargs.count("sweetadreno_power") == 0 && stage < 2)) ||
	    g_scenario == "prepowered" || g_scenario == "mapfail") {
		int ok = g_writes.empty();
		printf("ASSERT zero-writes: %s\n", ok ? "ok" : "FAIL（这条路径不该写寄存器）");
		bad |= !ok;
	}
	if (getenv("K60_DUMP_WRITES")) {
		for (auto &w : g_writes) printf("  write| %s\n", w.c_str());
	}
	return bad;
}
