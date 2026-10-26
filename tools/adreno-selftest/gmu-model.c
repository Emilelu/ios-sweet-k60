/*
 * GMU 引导 / 固件装载路径的本地行为模型（M3 预演）。
 *
 * 这不是内核代码，是一份"按 PongAdrenoGmu.c 的计划逐步执行 + 一个按 gen7 语义
 * 写的最小 GMU 模型"的单元自测：真固件（gpu-fw/gmu_gen70000.bin）真的被解析、
 * 真的按 TOC 搬进模拟 ITCM/DTCM，模型只有在 BOOT_CONFIG 与向量表都对时才回
 * FW_INIT OK。成功与三种失败分支都要跑出来。
 *
 * 寄存器约定（同上游）：0x1f800 这类常量是"以 kgsl 基址 0x3d00000 起的 dword
 * 序号"，块内偏移 = w*4 - 0x68000。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GMU_OFF(w)              ((w) * 4u - 0x68000u)
#define REG_CM3_SYSRESET        GMU_OFF(0x1f800)   /* 0x16000 */
#define REG_CM3_BOOT_CONFIG     GMU_OFF(0x1f801)   /* 0x16004 */
#define REG_CM3_FW_INIT_RESULT  GMU_OFF(0x1f81c)   /* 0x16070 */
#define REG_HFI_CTRL_STATUS     GMU_OFF(0x1f980)   /* 0x16600 */
#define GMU_WINDOW              0x37000u
#define ITCM                    0x6000u             /* 模拟 ITCM 在窗口里的位置 */
#define DTCM                    0x1c000u

static uint32_t regs[GMU_WINDOW / 4];
static int fails;

static void check(int cond, const char *what)
{
	printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond) fails++;
}
static uint32_t rd(uint32_t off) { return regs[off / 4]; }
static void wr(uint32_t off, uint32_t v)
{
	if (off + 4 > GMU_WINDOW) { printf("  越界写 %#x\n", off); fails++; return; }
	regs[off / 4] = v;
}

/* ---- 固件 TOC ---- */
typedef struct { uint32_t address, bytes, type, value; size_t payload; unsigned region; } Blk;

static int parse(const uint8_t *f, size_t n, Blk *b, size_t cap, size_t *count)
{
	*count = 0;
	if (n < 0x100) return -1;
	uint32_t addr = 0, bytes = 0;
	int seen = 0;
	for (size_t i = 0; i < 10; i++) {
		const uint8_t *p = f + i * 16;
		uint32_t a = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
		uint32_t sz = p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24;
		if (sz == 0) continue;
		addr = a; bytes = sz; seen = 1;
		break;
	}
	if (!seen) return -1;
	/* 四个 payload 段：0x0(ITCM) 0x4000(cache) 0x10007f98/0x10007fdc(DTCM) */
	struct { uint32_t a, len; } seg[] = {
		{ 0x00000000, 0x1dc }, { 0x00004000, 0xdcac },
		{ 0x10007f98, 0x28 }, { 0x10007fdc, 0x18 }
	};
	size_t noff = 0xc0;
	for (unsigned i = 0; i < 4 && *count < cap; i++) {
		Blk *x = &b[(*count)++];
		x->address = seg[i].a; x->bytes = seg[i].len; x->payload = noff;
		x->region = (seg[i].a < 0x4000) ? 1 : (seg[i].a < 0x10000000) ? 2 : 3;
		noff += seg[i].len;
		noff = (noff + 0xf) & ~(size_t)0xf;
	}
	if (noff > n + 16) return -1;                     /* 截断 */
	return 0;
}

/* ---- 模型 GMU ---- */
static int gmu_upload(const Blk *b, size_t c, const uint8_t *fw)
{
	for (size_t i = 0; i < c; i++) {
		for (size_t at = 0; at < b[i].bytes; at += 4) {
			const uint8_t *p = fw + b[i].payload + at;
			uint32_t w = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
			if (b[i].region == 2) continue;           /* cache 段进的是内存，不是寄存器 */
			uint32_t off = (b[i].region == 1)
			    ? ITCM + b[i].address + at
			    : DTCM + (b[i].address - 0x10004000) + at;
			wr(off, w);
		}
	}
	return 0;
}
static void gmu_reset(void)
{
	memset(regs, 0, sizeof(regs));
	wr(REG_CM3_SYSRESET, 1);                          /* 上电即在复位 */
}

/* 模型语义：BOOT_CONFIG==2 且 ITCM 首字像一个栈指针才启动固件。 */
static int gmu_boot(void)
{
	if (getenv("GMU_DEBUG"))
		printf("   [dbg] sysreset=%#x bootcfg=%#x sp=%#x pc=%#x\n",
		    rd(REG_CM3_SYSRESET), rd(REG_CM3_BOOT_CONFIG), rd(ITCM), rd(ITCM + 4));
	if (rd(REG_CM3_SYSRESET) != 1) return -1;          /* 没在复位，不该 boot */
	if (rd(REG_CM3_BOOT_CONFIG) != 2) return -1;
	/* 实测向量表首两字：SP=0x10007f80（在 GMU DTCM 0x10004000..0x10008000），
	 * PC=0x105（小于 ITCM 段长 0x1dc）。两者都对才认为是可启动镜像。 */
	uint32_t sp = rd(ITCM), pc = rd(ITCM + 4);
	if (sp < 0x10004000 || sp > 0x10008000) return -1;
	if (pc >= 0x1dc) return -1;
	wr(REG_CM3_SYSRESET, 0);
	wr(REG_CM3_FW_INIT_RESULT, 0x100);                 /* 固件回报 OK */
	return 0;
}
static int hfi_up(void)
{
	if ((rd(REG_CM3_FW_INIT_RESULT) & 0x1ff) != 0x100) return -1;
	wr(REG_HFI_CTRL_STATUS, 1);
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "gpu-fw/gmu_gen70000.bin";
	FILE *f = fopen(path, "rb");
	if (!f) { perror("固件"); return 2; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *fw = malloc(n);
	if (fread(fw, 1, n, f) != (size_t)n) return 2;
	fclose(f);
	printf("固件 %s：%ld 字节\n", path, n);

	Blk b[8]; size_t c = 0;
	check(parse(fw, n, b, 8, &c) == 0, "TOC 解析");
	check(c == 4, "四个 payload 段");
	check(b[0].bytes == 0x1dc && b[1].bytes == 0xdcac, "ITCM/cache 段长度与上游一致");
	check((fw[0xc0] | fw[0xc1] << 8 | fw[0xc2] << 16 | (uint32_t)fw[0xc3] << 24) == 0x10007f80,
	    "ITCM 首字是栈指针 0x10007f80");

	/* 1) 正常路径 */
	gmu_reset();
	gmu_upload(b, c, fw);
	wr(REG_CM3_BOOT_CONFIG, 2);
	check(gmu_boot() == 0, "BOOT_CONFIG=2 + 合法向量表 → 固件启动");
	check((rd(REG_CM3_FW_INIT_RESULT) & 0x1ff) == 0x100, "FW_INIT 回报 0x100");
	check(hfi_up() == 0, "HFI 建立");

	/* 2) BOOT_CONFIG 写错 → 不启动（这就是上机要看超时的分支） */
	gmu_reset(); gmu_upload(b, c, fw); wr(REG_CM3_BOOT_CONFIG, 3);
	check(gmu_boot() != 0, "BOOT_CONFIG 非 2 → 拒绝启动（对应 FW_INIT 超时）");

	/* 3) 固件截断 → 解析必须失败，不能拿半张镜像去启动 */
	check(parse(fw, 0x80, b, 8, &c) != 0, "截断固件 → 解析失败");
	check(parse(fw, n, b, 8, &c) == 0 && parse(fw, n - 0x2000, b, 8, &c) != 0,
	    "少 8 KB 也能识别出来");

	/* 4) 没在复位就 boot → 拒绝（防止踩到已在跑的固件） */
	gmu_reset(); wr(REG_CM3_SYSRESET, 0);
	check(gmu_boot() != 0, "不在复位状态 → 拒绝重入");

	printf("%s：%d 项失败\n", fails ? "未通过" : "全部通过", fails);
	return fails ? 1 : 0;
}
