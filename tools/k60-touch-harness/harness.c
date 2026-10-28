/*
 * 宿主侧冒烟台：把 bsd/dev/k60_touch_spi.c 整个 include 进来，用一块假 MMIO
 * 跑那五步，检查日志行、寄存器写入和返回值。假硬件只做内存读写，所以要测的
 * 是代码路径（打包/拆包、轮询退出、门槛），不是寄存器语义。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 和被测文件的 include 顺序一致：这样替身函数的原型和真声明对得上。 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

/* 假 MMIO：一个物理地址一张页，ml_io_map 返回同一块内存。 */
struct page {
	unsigned long pa;
	unsigned long size;
	unsigned char *mem;
};

#define MAXPAGES 16
static struct page pages[MAXPAGES];
static int npages;
static char argname[64];
static int argval;
static int hz_counter;

static unsigned char *
model_page(unsigned long pa, unsigned long size)
{
	int i;

	for (i = 0; i < npages; i++) {
		if (pages[i].pa == pa) {
			return pages[i].mem;
		}
	}
	if (npages == MAXPAGES) {
		abort();
	}
	pages[npages].pa = pa;
	pages[npages].size = size;
	pages[npages].mem = calloc(1, size);
	return pages[npages++].mem;
}

static void
model_put32(unsigned long pa, unsigned int off, uint32_t val)
{
	unsigned char *mem = model_page(pa, 0x4000);

	memcpy(mem + off, &val, 4);
}

static uint32_t
model_get32(unsigned long pa, unsigned int off)
{
	unsigned char *mem = model_page(pa, 0x4000);
	uint32_t val;

	memcpy(&val, mem + off, 4);
	return val;
}

/* ---- 内核 API 的替身 ---- */
vm_offset_t
ml_io_map(vm_offset_t pa, vm_size_t size)
{
	return (vm_offset_t)model_page((unsigned long)pa, (unsigned long)size);
}

void
delay(int usec)
{
	(void)usec;
}

int
PE_parse_boot_argn(const char *name, void *ptr, int len)
{
	(void)len;
	if (argname[0] != 0 && strcmp(name, argname) == 0) {
		*(int *)ptr = argval;
		return 1;
	}
	return 0;
}

static int starts;

kern_return_t
kernel_thread_start(thread_continue_t cont, void *param, thread_t *out)
{
	(void)cont;
	(void)param;
	starts++;
	*out = (thread_t)0;
	return KERN_SUCCESS;
}

void
thread_deallocate(thread_t t)
{
	(void)t;
}

thread_t
current_thread(void)
{
	return (thread_t)0;
}

kern_return_t
thread_terminate(thread_t t)
{
	(void)t;
	return 0;
}

wait_result_t
thread_block(thread_continue_t cont)
{
	(void)cont;
	return 0;
}

wait_result_t
assert_wait_deadline(event_t event, wait_interrupt_t interruptible,
    uint64_t deadline)
{
	(void)event;
	(void)interruptible;
	(void)deadline;
	return 0;
}

void
clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *result)
{
	*result = (uint64_t)interval * scale;
}

void
sweet_log_flush_now(void)
{
	hz_counter++;
}

/* 被测代码。 */
#include "harness_src/k60_touch_spi.c"

#define TLMM(pin) (0x0f100000UL + 0x1000UL * (pin))

static int failures;

static void
check(int cond, const char *what)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond) {
		failures++;
	}
}

static void
seed_se(uint32_t fw, uint32_t status, uint32_t irq_status,
    uint32_t rx_sts, uint32_t rx_word)
{
	model_put32(0x00990000UL, 0x68, fw);
	model_put32(0x00990000UL, 0x40, status);
	model_put32(0x00990000UL, 0x610, irq_status);
	model_put32(0x00990000UL, 0x804, rx_sts);
	model_put32(0x00990000UL, 0x780, rx_word);
}

int
main(void)
{
	int r;
	unsigned char *se_before;

	/* 第 1 步：三根引脚的写入和回读。 */
	model_page(TLMM(K60_TOUCH_AVDD_GPIO), 0x1000);
	model_page(TLMM(K60_TOUCH_RESET_GPIO), 0x1000);
	model_page(TLMM(K60_TOUCH_IRQ_GPIO), 0x1000);
	k60.flush = 1;
	r = k60_stage_power();
	check(r == 0, "stage 1 returns 0");
	/* 假硬件不模拟焊盘，所以只看输出值那一位（真实硬件上 bit0 会跟着它）。 */
	check((model_get32(TLMM(K60_TOUCH_AVDD_GPIO), 0x4) & 2u) != 0,
	    "avdd driven high");
	check((model_get32(TLMM(K60_TOUCH_AVDD_GPIO), 0x0) & (1u << 9)) != 0,
	    "avdd CFG output enable set");
	check((model_get32(TLMM(K60_TOUCH_RESET_GPIO), 0x4) & 2u) != 0,
	    "reset released high");
	check((model_get32(TLMM(K60_TOUCH_RESET_GPIO), 0x0) & 3u) ==
	    K60_TLMM_PULL_NONE, "reset has no pull");
	check((model_get32(TLMM(K60_TOUCH_IRQ_GPIO), 0x0) & 3u) ==
	    K60_TLMM_PULL_DOWN, "irq pulled down");
	check((model_get32(TLMM(K60_TOUCH_IRQ_GPIO), 0x0) & (1u << 9)) == 0,
	    "irq is an input");

	/* 第 2 步：五个分支，假硬件按读改写回，位应该都立住。 */
	model_page(0x00100000UL, 0x70000);
	k60.stage = 5;
	r = k60_stage_clocks();
	check(r == 0, "stage 2 returns 0");
	check((model_get32(0x00100000UL, 0x62008) &
	    ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 14))) ==
	    ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 14)),
	    "all five clock bits latched");

	/* 第 3 步：SPI 固件 -> 过；I2C 固件 -> 停。 */
	seed_se(0x102, 0, 0, 0, 0);
	r = k60_stage_geni();
	check(r == 0, "stage 3 accepts protocol 1 (SPI)");
	seed_se(0x302, 0, 0, 0, 0);
	r = k60_stage_geni();
	check(r == -1, "stage 3 rejects protocol 3 (I2C)");

	/* 第 4 步：回环。RX FIFO 里放我们自己打包的那一个字的镜像。 */
	seed_se(0x102, 0, 0x1, 1, 0x78563412);
	r = k60_stage_spi();
	check(r == 0, "stage 4 loopback matches 0x12 0x34 0x56 0x78");
	check((model_get32(0x00990000UL, 0x22c) & 1u) == 0, "loopback turned off");
	/* 超时路径：完成位永远不来，必须退出而不是挂住。 */
	seed_se(0x102, 0, 0, 0, 0);
	r = k60_stage_spi();
	check(r == -1, "stage 4 fails (not hangs) when DONE never comes");

	/* 第 5 步：产品 id 两种框法。 */
	seed_se(0x102, 0, 0x1, 1, 0x00313139);	/* "911" + 0x00 */
	r = k60_stage_id();
	check(r == 0, "stage 5 reports both framings completed");

	/* 开关与门槛：默认只跑到第 1 步，GCC 和 SE 一个字节都不许碰。 */
	model_page(0x00100000UL, 0x70000);
	memset(model_page(0x00100000UL, 0x70000), 0, 0x70000);
	k60.started = false;
	k60.stage = 1;
	k60.flush = 0;
	starts = 0;
	snprintf(argname, sizeof(argname), "k60touchoff");
	argval = 1;
	k60_touch_spi_init();
	check(starts == 0, "k60touchoff=1 never starts the thread");
	argname[0] = 0;
	k60.started = false;
	k60_touch_spi_init();
	check(starts == 1, "without k60touchoff the thread starts");
	check(k60.stage == K60_STAGE_POWER, "default stage limit is 1 (power only)");

	/* SE 那一页前面的测试已经写过，所以整页前后比对，而不是看某个寄存器。 */
	se_before = malloc(0x4000);
	memcpy(se_before, model_page(0x00990000UL, 0x4000), 0x4000);
	k60_touch_thread(NULL, 0);
	check(model_get32(0x00100000UL, 0x62008) == 0,
	    "stage limit 1 leaves GCC untouched");
	check(memcmp(se_before, model_page(0x00990000UL, 0x4000), 0x4000) == 0,
	    "stage limit 1 leaves the SE untouched");
	free(se_before);

	printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
