/*
 * 0240+（bsd/dev/k60_touch*.c）的宿主冒烟台。
 *
 * 假硬件是一组按物理地址索引的内存页；K60_TOUCH_RD/WR 在 K60_TOUCH_HOST_TEST
 * 下走 k60_touch_test_read/write，于是这里能：
 *   - 拦住 SE4 的寄存器，模拟一个 GENI 引擎（命令、TX FIFO、RX FIFO、
 *     DONE 位、长度寄存器），
 *   - 在 GENI 引擎后面挂一颗假 Goodix（Berlin 与 GT9xx 两套地址表都认），
 *   - 拦住 RSC 的 TCS，模拟"写 CONTROL|TRIGGER 之后 IRQ_STATUS bit0 立起来"，
 *   - 拦住 TLMM 引脚页，检查写进去的位域。
 *
 * 每个场景是一个独立的进程（build2.sh 逐个跑），所以驱动里的静态状态不需要
 * 复位。判据是日志行（printf 被截获）与 fake MMIO 的最终内容。
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/k60_touch.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

/* ---- 断言 ---- */
static int failures;
#define CHECK(cond, ...) do { \
	if (cond) { printf_stderr("ok   " __VA_ARGS__); } \
	else { failures++; printf_stderr("FAIL " __VA_ARGS__); } \
} while (0)

static void
printf_stderr(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

/* ---- 假 MMIO ---- */
#define MAXPAGES 192

struct page {
	uint64_t pa;
	uint64_t size;
	uint8_t *mem;
};

static struct page pages[MAXPAGES];
static int npages;
static int se_touched;
static int unmapped_touched;

static struct page *
page_map(uint64_t pa, uint64_t size)
{
	int i;

	for (i = 0; i < npages; i++) {
		if (pages[i].pa == pa && pages[i].size >= size) {
			return &pages[i];
		}
	}
	if (npages == MAXPAGES) {
		fprintf(stderr, "harness: page table full\n");
		exit(2);
	}
	pages[npages].pa = pa;
	pages[npages].size = size;
	pages[npages].mem = calloc(1, size);
	if (!pages[npages].mem) {
		exit(2);
	}
	return &pages[npages++];
}

static struct page *
page_of_host(const void *p, uint64_t *off)
{
	uintptr_t a = (uintptr_t)p;
	int i;

	for (i = 0; i < npages; i++) {
		uintptr_t base = (uintptr_t)pages[i].mem;

		if (a >= base && a < base + pages[i].size) {
			*off = a - base;
			return &pages[i];
		}
	}
	unmapped_touched++;
	if (getenv("HARNESS_DBG")) {
		fprintf(stderr, "unmapped access at %p\n", p);
	}
	return NULL;
}

static uint8_t *
phys_ptr(uint64_t pa, uint64_t size)
{
	return page_map(pa, size)->mem;
}

/* ---- 假 Goodix ---- */
static uint8_t chipmem[0x20000];
static int chip_writes;

static uint8_t
chip_read8(uint32_t address)
{
	return chipmem[address & 0x1ffff];
}

static void
chip_write8(uint32_t address, uint8_t value)
{
	chipmem[address & 0x1ffff] = value;
	chip_writes++;
}

/* ---- 假 GENI 引擎 ---- */
#define SE_BASE 0x990000u

struct geni {
	int loopback;
	uint32_t expected;	/* 字节数（驱动默认按字节写 TRANS_LEN） */
	uint32_t got;
	uint8_t tx[2048];
	uint32_t rx_words[512];
	uint32_t rx_count;
	uint32_t rx_read;
	uint32_t irq;
};
static struct geni geni;
static int geni_never_completes;

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3];
}

/* 假芯片对一笔完整传输的回答。 */
static void
chip_respond(const uint8_t *tx, uint32_t length, uint8_t *rx)
{
	uint32_t i, addr, shift = 0;
	const uint8_t *cmd = tx;

	memset(rx, 0, length);
	if (geni.loopback) {
		memcpy(rx, tx, length);
		return;
	}
	if (length == 0) {
		return;
	}
	if (tx[0] == 0x20 && length > 1) {
		shift = 1;
		cmd = tx + 1;
	}
	if (cmd[0] == 0xf1 && length >= 8 + shift) {
		addr = be32(cmd + 1);
		for (i = 0; i + 8 + shift < length; i++) {
			rx[8 + shift + i] = chip_read8(addr + i);
		}
		return;
	}
	if (cmd[0] == 0xf0 && length >= 5 + shift) {
		addr = be32(cmd + 1);
		for (i = 0; i + 5 + shift < length; i++) {
			chip_write8(addr + i, cmd[5 + i]);
		}
		return;
	}
	if ((cmd[0] & 0x80) != 0 && length >= 2 + shift) {
		/* GT9xx 读：0x80 | 高字节；寄存器都在 0x8000..0x8fff。 */
		addr = ((uint32_t)cmd[0] << 8) | cmd[1];
		for (i = 0; i + 2 + shift < length; i++) {
			rx[2 + shift + i] = chip_read8(addr + i);
		}
		return;
	}
	if (length >= 2 + shift) {
		/* GT9xx 写：高字节的 bit7 被驱动清掉了，补回来。 */
		addr = ((uint32_t)(cmd[0] | 0x80) << 8) | cmd[1];
		for (i = 0; i + 2 + shift < length; i++) {
			chip_write8(addr + i, cmd[2 + i]);
		}
		return;
	}
}

static void
geni_complete(void)
{
	uint8_t rx[2048];
	uint32_t i, words;

	chip_respond(geni.tx, geni.expected, rx);
	words = (geni.expected + 3) / 4;
	geni.rx_count = words;
	geni.rx_read = 0;
	for (i = 0; i < words; i++) {
		uint32_t w = 0;
		int j;

		for (j = 0; j < 4; j++) {
			uint32_t k = i * 4 + j;

			if (k < geni.expected) {
				w |= (uint32_t)rx[k] << (8 * j);
			}
		}
		geni.rx_words[i] = w;
	}
	geni.irq = 1;	/* M_IRQ_STATUS.DONE */
}

/* ---- RSC：一次 TRIGGER 就完成 ---- */
static int rsc_triggers;

/* ---- 截获的 printf ---- */
static char logbuf[256 * 1024];
static size_t loglen;
static int log_echo;

int
printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
	va_end(ap);
	if (n > 0) {
		loglen += (size_t)n;
	}
	if (log_echo) {
		fputs(logbuf + loglen - (size_t)n, stderr);
	}
	return n;
}

static int
log_has(const char *needle)
{
	return strstr(logbuf, needle) != NULL;
}

/* ---- boot-arg ---- */
static struct {
	char name[32];
	int value;
} bootargs[16];
static int nbootargs;

static void
boot_arg(const char *name, int value)
{
	snprintf(bootargs[nbootargs].name, sizeof(bootargs[0].name), "%s",
	    name);
	bootargs[nbootargs].value = value;
	nbootargs++;
}

int
PE_parse_boot_argn(const char *name, void *out, int max_len)
{
	int i;

	for (i = 0; i < nbootargs; i++) {
		if (strcmp(bootargs[i].name, name) == 0) {
			if (max_len >= (int)sizeof(int)) {
				memcpy(out, &bootargs[i].value, sizeof(int));
			} else {
				memcpy(out, &bootargs[i].value, (size_t)max_len);
			}
			return 1;
		}
	}
	return 0;
}

/* ---- 假 ADT：只有 /k60-touch 一个节点 ------------------------------------ */
struct fake_prop {
	char name[48];
	uint8_t value[64];
	unsigned size;
};
static struct fake_prop dtprops[12];
static int ndtprops;
static int dt_present;

static void
dt_set(const char *name, const void *value, unsigned size)
{
	struct fake_prop *p;

	if (ndtprops == (int)(sizeof(dtprops) / sizeof(dtprops[0])) ||
	    size > sizeof(p->value)) {
		fprintf(stderr, "harness: dt table full\n");
		exit(2);
	}
	p = &dtprops[ndtprops++];
	snprintf(p->name, sizeof(p->name), "%s", name);
	memcpy(p->value, value, size);
	p->size = size;
}

static void
dt_u32(const char *name, uint32_t value)
{
	dt_set(name, &value, sizeof(value));
}

static void
build_touch_node(void)
{
	uintptr_t reg[6];

	dt_present = 1;
	reg[0] = 0x00990000;
	reg[1] = 0x4000;
	reg[2] = 0x00100000;
	reg[3] = 0x70000;
	reg[4] = 0x0f100000;
	reg[5] = 0x1000;
	dt_set("reg", reg, sizeof(reg));
	dt_u32("goodix,reset-gpio", 20);
	dt_u32("goodix,irq-gpio", 21);
	dt_u32("goodix,avdd-gpio", 71);
	dt_u32("goodix,panel-max-x", 14400);
	dt_u32("goodix,panel-max-y", 32000);
	dt_u32("spi-max-frequency", 15000000);
}

DTStatus
SecureDTLookupEntry(const DTEntry searchPoint, const char *pathName,
    DTEntry *foundEntry)
{
	(void)searchPoint;
	if (!dt_present || strcmp(pathName, "/k60-touch") != 0) {
		return 1;
	}
	*foundEntry = (DTEntry)&dt_present;
	return kSuccess;
}

DTStatus
SecureDTGetProperty(DTEntry entry, const char *propertyName,
    void const **propertyValue, unsigned int *propertySize)
{
	int i;

	(void)entry;
	for (i = 0; i < ndtprops; i++) {
		if (strcmp(dtprops[i].name, propertyName) == 0) {
			*propertyValue = dtprops[i].value;
			*propertySize = dtprops[i].size;
			return kSuccess;
		}
	}
	return 1;
}

/* ---- 内核 API 替身 ---- */
static uint64_t fake_now;
static int sleeping;
static jmp_buf thread_exit;
static int thread_ran, thread_terminated;
static thread_continue_t thread_cont;
static void *thread_param;
static int rsc_maps;	/* ml_io_map 命中 RSC 的次数：k60rpmhoff=1 必须是 0 */

vm_offset_t
ml_io_map(vm_offset_t pa, vm_size_t size)
{
	if (pa == 0x17a00000u || pa == 0x17a20000u) {
		rsc_maps++;
	}
	return (vm_offset_t)(uintptr_t)phys_ptr((uint64_t)pa, (uint64_t)size);
}

uint64_t
mach_absolute_time(void)
{
	fake_now += 50000;	/* 每读一次走 50 µs，超时路径不会真的等 */
	return fake_now;
}

void
clock_interval_to_deadline(uint32_t interval, uint32_t scale,
    uint64_t *result)
{
	*result = fake_now + (uint64_t)interval * scale;
}

void
delay(int usec)
{
	fake_now += (uint64_t)usec * 1000u;
}

void
delay_for_interval(uint32_t interval, uint32_t scale)
{
	fake_now += (uint64_t)interval * scale;
}

thread_t
current_thread(void)
{
	return (thread_t)1;
}

kern_return_t
kernel_thread_start(thread_continue_t cont, void *param, thread_t *out)
{
	thread_cont = cont;
	thread_param = param;
	thread_ran = 1;
	if (out) {
		*out = (thread_t)1;
	}
	return KERN_SUCCESS;
}

void
thread_deallocate(thread_t t)
{
	(void)t;
}

kern_return_t
thread_terminate(thread_t t)
{
	(void)t;
	thread_terminated = 1;
	return 0;
}

wait_result_t
assert_wait_deadline(event_t event, wait_interrupt_t interruptible,
    uint64_t deadline)
{
	(void)event;
	(void)interruptible;
	(void)deadline;
	sleeping = 1;
	return 0;
}

wait_result_t
thread_block(thread_continue_t continuation)
{
	(void)continuation;
	if (sleeping) {
		sleeping = 0;
		return 0;
	}
	/* 驱动在 stop 路径里先 thread_terminate 再 thread_block：跳回测试。 */
	longjmp(thread_exit, 1);
}

static int flush_calls;
void
sweet_log_flush_now(void)
{
	flush_calls++;
}

/* HID 上报：录下来。 */
struct hid_event {
	int finger, down, x, y;
};
static struct hid_event hid[64];
static int nhid;

void
sweet_hid_touch(int finger, int down, int x, int y)
{
	if (nhid < (int)(sizeof(hid) / sizeof(hid[0]))) {
		hid[nhid].finger = finger;
		hid[nhid].down = down;
		hid[nhid].x = x;
		hid[nhid].y = y;
		nhid++;
	}
}

static uint32_t
rd32(uint64_t pa, uint32_t off)
{
	uint8_t *p = phys_ptr(pa, 0x1000);
	uint32_t v;

	memcpy(&v, p + off, 4);
	return v;
}

static void
wr32(uint64_t pa, uint32_t off, uint32_t v)
{
	uint8_t *p = phys_ptr(pa, 0x1000);

	memcpy(p + off, &v, 4);
}

/* ---- K60_TOUCH_RD/WR 的替身（见 bsd/sys/k60_touch.h）---- */
uint32_t
k60_touch_test_read(uintptr_t address)
{
	struct page *page;
	uint64_t off;
	uint32_t v;

	page = page_of_host((const void *)address, &off);
	if (page == NULL) {
		return 0xffffffffu;
	}
	if (page->pa == SE_BASE) {
		se_touched++;
		switch ((uint32_t)off) {
		case 0x040:	/* GENI_STATUS：引擎空闲 */
			return 0;
		case 0x068:	/* FW_REVISION：version 2, protocol 1 (SPI) */
			return 0x00000102u;
		case 0x064:
			return 0;
		case 0x0e24:	/* HW_PARAM_0：[21:16] 是 6 位，64 会溢出成 0 */
			return 32u << 16;
		case 0x610:	/* M_IRQ_STATUS */
			return geni.irq;
		case 0x780:	/* RX FIFO */
			if (geni.rx_read < geni.rx_count) {
				return geni.rx_words[geni.rx_read++];
			}
			return 0;
		case 0x804: {	/* RX FIFO 状态：[24:0] 字数，最多一个 FIFO 深 */
			uint32_t left = geni.rx_count - geni.rx_read;

			if (left > 64u) {
				left = 64u;
			}
			return left & 0x01ffffffu;
		}
		case 0x800:	/* TX FIFO 状态：永远有空位 */
			return 0;
		case 0x22c:	/* SPI_LOOPBACK 回读 */
			return (uint32_t)geni.loopback;
		}
	}
	memcpy(&v, page->mem + off, 4);
	return v;
}

void
k60_touch_test_write(uintptr_t address, uint32_t value)
{
	struct page *page;
	uint64_t off;
	uint64_t pa;

	page = page_of_host((const void *)address, &off);
	if (page == NULL) {
		return;
	}
	pa = page->pa;
	if (pa == 0x100000u && (uint32_t)off == 0x274e4u) {
		/* RCG: UPDATE 位写完自清；ROOT_EN 清掉 ROOT_OFF。 */
		if (value & 1u) {
			value &= ~1u;
		}
		if (value & 2u) {
			value &= ~(1u << 31);
		}
		memcpy(page->mem + off, &value, 4);
		return;
	}
	if (pa == SE_BASE) {
		se_touched++;
		switch ((uint32_t)off) {
		case 0x22c:	/* SPI_LOOPBACK */
			geni.loopback = (int)value;
			break;
		case 0x600:	/* M_CMD0 */
			if ((value >> 27) == 3u) {
				geni.expected = rd32(SE_BASE, 0x270);
				geni.got = 0;
				geni.rx_count = geni.rx_read = 0;
				geni.irq = 0;
			}
			break;
		case 0x700: {	/* TX FIFO */
			uint32_t i;

			for (i = 0; i < 4; i++) {
				if (geni.got < geni.expected &&
				    geni.got < sizeof(geni.tx)) {
					geni.tx[geni.got++] =
					    (uint8_t)(value >> (8 * i));
				}
			}
			if (geni.expected != 0 && geni.got >= geni.expected &&
			    !geni_never_completes) {
				geni_complete();
			}
			break;
		}
		case 0x618:	/* M_IRQ_CLEAR */
			geni.irq = 0;
			break;
		case 0x604:	/* M_CMD_CTRL */
			break;
		}
		memcpy(page->mem + off, &value, 4);
		return;
	}
	if (pa == 0x17a20000u) {
		/* TCS：CONTROL 带 TRIGGER 就完成，IRQ_STATUS bit0 = 1。 */
		if ((uint32_t)off == 0xd00u + 0x14u &&
		    (value & (1u << 24)) != 0) {
			rsc_triggers++;
			wr32(pa, 0xd00u + 0x04u, 1);
		}
	}
	memcpy(page->mem + off, &value, 4);
}

/* ---- 场景用的假硬件 ---- */
static void
init_pages(void)
{
	int pin;

	phys_ptr(SE_BASE, 0x4000);
	phys_ptr(0x100000u, 0x70000);
	phys_ptr(0x17a20000u, 0x10000);
	phys_ptr(0x80860000u, 0x20000);
	for (pin = 0; pin < 128; pin++) {
		phys_ptr(0x0f100000u + 0x1000u * (uint64_t)pin, 0x1000);
	}
	/* RSC 的槽位空闲：pong 的循环等的是 STATUS != 0。 */
	wr32(0x17a20000u, 0xd00u + 0x18u, 1);
}

static void
fake_clocks(int on)
{
	uint32_t off = on ? 0 : (1u << 31);

	wr32(0x100000u, 0x62008u, 0);
	wr32(0x100000u, 0x27004u, off);
	wr32(0x100000u, 0x27008u, off);
	wr32(0x100000u, 0x274dcu, off);
	wr32(0x100000u, 0x274e4u, on ? 0 : (1u << 31));	/* ROOT_OFF */
	wr32(0x100000u, 0x274e8u, 0);			/* src 0 = XO */
}

/* 一颗 Berlin 芯片：固件版本、IC info（含 pong 的 misc 段）、触点区。 */
#define FAKE_TOUCH_ADDR 0x11000u

static void
build_berlin_chip(void)
{
	uint8_t fw[28];
	uint8_t info[128];
	uint32_t off, sum, i;
	uint32_t len;

	memset(fw, 0, sizeof(fw));
	memcpy(fw, "911", 3);
	memcpy(fw + 10, "M11A0001", 8);
	fw[18] = 1;
	fw[23] = 0x0a;
	sum = 0;
	for (i = 0; i < sizeof(fw) - 2; i++) {
		sum += fw[i];
	}
	fw[26] = (uint8_t)sum;
	fw[27] = (uint8_t)(sum >> 8);
	for (i = 0; i < sizeof(fw); i++) {
		chip_write8(0x10014u + i, fw[i]);
	}

	memset(info, 0, sizeof(info));
	len = 120;
	info[0] = (uint8_t)len;
	info[1] = (uint8_t)(len >> 8);
	off = 2 + 16 + 10 + 4;
	for (i = 0; i < 5; i++) {
		info[off] = 0;	/* 每张表 0 项，刚好够 parse 走过 */
		off += 1;
	}
	/* misc：+44 触点地址、+48/+50 = 8、+52/+54 坐标上限。 */
	info[off + 44] = FAKE_TOUCH_ADDR & 0xff;
	info[off + 45] = (FAKE_TOUCH_ADDR >> 8) & 0xff;
	info[off + 46] = (FAKE_TOUCH_ADDR >> 16) & 0xff;
	info[off + 47] = (FAKE_TOUCH_ADDR >> 24) & 0xff;
	info[off + 48] = 8;
	info[off + 50] = 8;
	info[off + 52] = 0x40;	/* 0x3840 = 14400，与 DT 一致 */
	info[off + 53] = 0x38;
	info[off + 54] = 0x00;	/* 0x7d00 = 32000 */
	info[off + 55] = 0x7d;
	sum = 0;
	for (i = 0; i + 2 < len; i++) {
		sum += info[i];
	}
	info[len - 2] = (uint8_t)sum;
	info[len - 1] = (uint8_t)(sum >> 8);
	for (i = 0; i < len; i++) {
		chip_write8(0x10070u + i, info[i]);
	}
}

/* 写一份 Berlin 报告：count 个触点，坐标 x/y。 */
static void
build_berlin_report(uint32_t status, uint32_t count, uint32_t x, uint32_t y)
{
	uint8_t report[8 + 8 * 10 + 2];
	uint32_t i, sum;

	memset(report, 0, sizeof(report));
	report[0] = (uint8_t)status;
	report[1] = 0x01;
	report[2] = (uint8_t)count;
	sum = 0;
	for (i = 0; i < 6; i++) {
		sum += report[i];
	}
	report[6] = (uint8_t)sum;
	report[7] = (uint8_t)(sum >> 8);
	/* 触点区单独一个和，覆盖 count*8 + 2 字节。 */
	for (i = 0; i < count; i++) {
		uint8_t *t = report + 8 + 8 * i;

		t[0] = (uint8_t)(i << 4);
		t[2] = (uint8_t)x;
		t[3] = (uint8_t)(x >> 8);
		t[4] = (uint8_t)y;
		t[5] = (uint8_t)(y >> 8);
	}
	sum = 0;
	for (i = 0; i < 8 * count; i++) {
		sum += report[8 + i];
	}
	report[8 + 8 * count] = (uint8_t)sum;
	report[8 + 8 * count + 1] = (uint8_t)(sum >> 8);
	for (i = 0; i < sizeof(report); i++) {
		chip_write8(FAKE_TOUCH_ADDR + i, report[i]);
	}
}

/* 老 ABI 的芯片：0x8140 产品 id、0x814e 状态、0x8150 触点。 */
static void
build_gt9xx_chip(void)
{
	const char *id = "9111";
	uint32_t i;

	/* Berlin 那边全 0：校验和不过，探针会退到 GT9xx。 */
	for (i = 0; i < 0x20000; i++) {
		chipmem[i] = 0;
	}
	for (i = 0; i < 4; i++) {
		chip_write8(0x8140u + i, (uint8_t)id[i]);
	}
	chip_write8(0x814eu, 0x80u);
	chip_write8(0x814fu, 0x00u);
}

static void
build_gt9xx_report(uint32_t count, uint32_t x, uint32_t y)
{
	uint32_t i;

	chip_write8(0x814eu, (uint8_t)(0x80u | count));
	for (i = 0; i < count; i++) {
		uint32_t base = 0x8150u + 8 * i;

		chip_write8(base + 0, (uint8_t)i);
		chip_write8(base + 1, (uint8_t)x);
		chip_write8(base + 2, (uint8_t)(x >> 8));
		chip_write8(base + 3, (uint8_t)y);
		chip_write8(base + 4, (uint8_t)(y >> 8));
	}
}

/* 一份能让命令数据库查到的 image：一个 slave、一条 "ldoc2"@0x40004。 */
static void
build_cmd_db(const char *name, uint32_t address, int present)
{
	uint8_t *db = phys_ptr(0x80860000u, 0x20000);
	uint8_t *entry;
	uint32_t header_offset = 0x100;
	uint32_t i;

	memset(db, 0, 0x20000);
	if (!present) {
		return;
	}
	db[0] = 0x01;
	db[4] = 0xdb;
	db[5] = 0x30;
	db[6] = 0x03;
	db[7] = 0x0c;
	/*
	 * struct k60_cmd_db_header：version(4) magic(4) slave[8](16 字节一个)
	 * checksum(4) reserved(4) -> data[] 从 144 字节开始。
	 * slave[0] = { id @8, header_offset @10, data_offset @12, count @14 }。
	 */
	db[8] = 4;
	db[10] = (uint8_t)header_offset;
	db[11] = (uint8_t)(header_offset >> 8);
	db[14] = 1;
	entry = db + 144 + header_offset;
	for (i = 0; i < 8 && name[i]; i++) {
		entry[i] = (uint8_t)name[i];
	}
	/* struct k60_cmd_db_entry：id(8) priority(8) address @16 length @20。 */
	entry[16] = (uint8_t)address;
	entry[17] = (uint8_t)(address >> 8);
	entry[18] = (uint8_t)(address >> 16);
	entry[19] = (uint8_t)(address >> 24);
	entry[20] = 4;
	entry[21] = 0;
}

static void
reset_chip(void)
{
	memset(chipmem, 0, sizeof(chipmem));
}

/* ---- 场景 ---- */
static void
run_thread(void)
{
	if (thread_cont == NULL) {
		return;
	}
	if (setjmp(thread_exit) == 0) {
		thread_cont(thread_param, 0);
	}
}

int
main(int argc, char **argv)
{
	const char *scenario = argc > 1 ? argv[1] : "";

	argc = argc;
	init_pages();
	log_echo = getenv("HARNESS_ECHO") != NULL;

	if (strcmp(scenario, "default_stage") == 0) {
		build_cmd_db("ldoc2", 0x40004u, 1);
		k60_touch_init();
		CHECK(thread_ran, "thread started with no boot-arg");
		CHECK(log_has("stage 2"), "default stage is 2");
		run_thread();
		CHECK(log_has("powered: avdd=1 reset=1 irq level 0"),
		    "power stage reports the pin state");
		CHECK(log_has("iovdd ldoc2 1800 mV mode 7 voted"),
		    "iovdd vote completed");
		CHECK(log_has("irq after iovdd"), "irq is re-read after the vote");
		CHECK(log_has("stage 2 done"), "stops at the stage limit");
		CHECK(!se_touched, "never touches SE4 at stage 2");
		/* 引脚位域：AVDD 输出使能、RESET 8 mA 输出、IRQ 输入下拉。 */
		CHECK(rd32(0x0f100000u + 0x1000u * 71, 0x0) == 0x200u,
		    "AVDD cfg = function 0, pull none, 2 mA, output (%#x)",
		    rd32(0x0f100000u + 0x1000u * 71, 0x0));
		CHECK(rd32(0x0f100000u + 0x1000u * 20, 0x0) == 0x2c0u,
		    "RESET cfg = 8 mA output (%#x)",
		    rd32(0x0f100000u + 0x1000u * 20, 0x0));
		/* 0245：IRQ 是开漏输出，必须上拉；下拉（0x1）会把电平读成假 0。 */
		CHECK(rd32(0x0f100000u + 0x1000u * 21, 0x0) == 0x3u,
		    "IRQ cfg = input, pull up (%#x)",
		    rd32(0x0f100000u + 0x1000u * 21, 0x0));
		/* RPMh：三条 4 字节写落在 ldoc2 的 +0/+4/+8。 */
		CHECK(rd32(0x17a20000u, 0xd00u + 0x34u) == 0x40004u,
		    "VRM voltage write goes to ldoc2+0");
		CHECK(rd32(0x17a20000u, 0xd00u + 0x38u) == 1800u,
		    "voltage is 1800 mV");
		CHECK(rd32(0x17a20000u, 0xd00u + 0x34u + 20u) == 0x40008u,
		    "VRM enable write goes to ldoc2+4");
		CHECK(rd32(0x17a20000u, 0xd00u + 0x38u + 20u) == 1u,
		    "enable = 1");
		CHECK(rd32(0x17a20000u, 0xd00u + 0x34u + 40u) == 0x4000cu,
		    "VRM mode write goes to ldoc2+8");
		CHECK(rd32(0x17a20000u, 0xd00u + 0x38u + 40u) == 7u,
		    "mode = 7 (HPM)");
		CHECK(rsc_triggers == 1, "exactly one RSC trigger");
	} else if (strcmp(scenario, "disabled") == 0) {
		boot_arg("k60touch", 0);
		k60_touch_init();
		CHECK(!thread_ran, "k60touch=0 starts no thread");
		CHECK(log_has("disabled by k60touch=0"), "says why it is off");
	} else if (strcmp(scenario, "stage0") == 0) {
		boot_arg("k60touchstage", 0);
		k60_touch_init();
		CHECK(!thread_ran, "k60touchstage=0 starts no thread");
	} else if (strcmp(scenario, "cmd_db_missing") == 0) {
		build_cmd_db("ldoc2", 0x40004u, 0);
		boot_arg("k60touchstage", 2);
		k60_touch_init();
		run_thread();
		CHECK(log_has("cmd_db has no db30030c magic"),
		    "missing command DB is reported");
		CHECK(log_has("iovdd failed") || log_has("iovdd vote failed"),
		    "the thread stops at the iovdd stage");
		CHECK(!se_touched, "still no SE access");
	} else if (strcmp(scenario, "clocks_off") == 0) {
		build_cmd_db("ldoc2", 0x40004u, 1);
		fake_clocks(0);
		boot_arg("k60touchstage", 4);
		k60_touch_init();
		run_thread();
		CHECK(log_has("clocks not ready"), "halts that never clear stop the boot");
		CHECK(!se_touched, "no SE access when the clocks are not ready");
	} else if (strcmp(scenario, "clocks_on") == 0) {
		build_cmd_db("ldoc2", 0x40004u, 1);
		fake_clocks(1);
		boot_arg("k60touchstage", 3);
		k60_touch_init();
		run_thread();
		CHECK(log_has("clocks ready"), "clocks report ready");
		CHECK(log_has("source 19200000"), "decodes XO as the source");
		CHECK(rd32(0x100000u, 0x62008u) ==
		    ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 14)),
		    "all five vote bits are set (%#x)", rd32(0x100000u, 0x62008u));
		CHECK(log_has("stage 3 done"), "stops before GENI at stage 3");
		CHECK(!se_touched, "stage 3 does not touch SE4");
	} else if (strcmp(scenario, "geni_timeout") == 0) {
		build_cmd_db("ldoc2", 0x40004u, 1);
		fake_clocks(1);
		geni_never_completes = 1;
		boot_arg("k60touchstage", 4);
		k60_touch_init();
		run_thread();
		CHECK(log_has("stage 3: gcc"), "reached the clock stage");
		CHECK(log_has("se4 fw 0x102"), "reads the SPI firmware revision");
		CHECK(log_has("spi xfer") && log_has("bytes failed"),
		    "a transfer that never completes fails instead of hanging");
		CHECK(rd32(0x990000u, 0x48u) == ((5u << 4) | 1u),
		    "SER divider = XO/4 MHz = 5 (%#x)",
		    rd32(0x990000u, 0x48u));
		CHECK(log_has("goodix id failed"), "and the thread stops there");
	} else if (strcmp(scenario, "berlin") == 0) {
		const char *problem = NULL;

		build_cmd_db("ldoc2", 0x40004u, 1);
		fake_clocks(1);
		build_berlin_chip();
		build_berlin_report(0x80, 1, 7200, 16000);
		CHECK(k60_touch_clocks_on() == 0, "clocks on");
		CHECK(k60_touch_geni_bringup(4000000u, &problem) == 0,
		    "SPI setup (%s)", problem ? problem : "ok");
		CHECK(k60_touch_goodix_probe(1) == 0, "Berlin probe succeeds");
		CHECK(k60_touch_goodix_abi() == 1, "Berlin ABI selected");
		CHECK(log_has("id: berlin fw chip \"911\""), "chip name decoded");
		CHECK(log_has("touch report at 0x11000"), "report address found");
		CHECK(log_has("axes 14400 x 32000"), "axes taken from IC info");
		CHECK(k60_touch_goodix_poll() == 0, "first poll returns a report");
		CHECK(nhid == 1 && hid[0].down && hid[0].finger == 0,
		    "one finger down");
		CHECK(hid[0].x == 720 && hid[0].y == 1600,
		    "7200/14400*1440 = 720 px, 16000/32000*3200 = 1600 px (%d,%d)",
		    hid[0].x, hid[0].y);
		/* 第二份报告没有触点：应当抬起，坐标沿用上一份。 */
		/*
		 * 没有触点的报告：状态字节非 0（0 = "还没读到东西"，驱动会
		 * 直接返回），TOUCH 位清掉，于是走抬起路径。
		 */
		build_berlin_report(0x01, 0, 0, 0);
		CHECK(k60_touch_goodix_poll() == 0, "second poll (no touch)");
		CHECK(nhid == 2 && !hid[1].down, "finger 0 lifted");
		CHECK(hid[1].x == 720 && hid[1].y == 1600,
		    "the lift carries the last coordinates (%d,%d)", hid[1].x,
		    hid[1].y);
	} else if (strcmp(scenario, "gt9xx") == 0) {
		const char *problem = NULL;

		build_cmd_db("ldoc2", 0x40004u, 1);
		fake_clocks(1);
		build_gt9xx_chip();
		build_gt9xx_report(1, 14400, 32000);
		CHECK(k60_touch_clocks_on() == 0, "clocks on");
		CHECK(k60_touch_geni_bringup(4000000u, &problem) == 0,
		    "SPI setup");
		CHECK(k60_touch_goodix_probe(1) == 0, "GT9xx probe succeeds");
		CHECK(k60_touch_goodix_abi() == 2, "GT9xx ABI selected");
		CHECK(log_has("id: gt9xx text 9111"), "product id read");
		CHECK(k60_touch_goodix_poll() == 0, "GT9xx poll");
		CHECK(nhid == 1 && hid[0].down, "one finger down");
		CHECK(hid[0].x == 1439 && hid[0].y == 3199,
		    "coordinates clamp to the panel (%d,%d)", hid[0].x,
		    hid[0].y);
		CHECK(chip_read8(0x814eu) == 0, "status byte cleared");
	} else if (strcmp(scenario, "no_chip") == 0) {
		const char *problem = NULL;

		build_cmd_db("ldoc2", 0x40004u, 1);
		fake_clocks(1);
		reset_chip();
		CHECK(k60_touch_clocks_on() == 0, "clocks on");
		CHECK(k60_touch_geni_bringup(4000000u, &problem) == 0,
		    "SPI setup");
		CHECK(k60_touch_goodix_probe(1) != 0, "a silent chip fails the probe");
		CHECK(log_has("abi none"), "and says so");
		CHECK(k60_touch_goodix_poll() != 0, "polling a silent chip fails");
	} else if (strcmp(scenario, "dt_node") == 0) {
		build_touch_node();
		k60_touch_init();
		CHECK(k60_touch_hw.from_dt == 1, "wiring came from /k60-touch");
		CHECK(log_has("K60 TOUCH: dt: se 0x990000+0x4000"),
		    "logs the SE window read from the node");
		CHECK(k60_touch_hw.se_base == 0x990000 &&
		    k60_touch_hw.se_window == 0x4000 &&
		    k60_touch_hw.gcc_base == 0x100000 &&
		    k60_touch_hw.gcc_window == 0x70000 &&
		    k60_touch_hw.tlmm_base == 0xf100000 &&
		    k60_touch_hw.tlmm_stride == 0x1000,
		    "all three windows decoded");
		CHECK(k60_touch_hw.gpio_reset == 20 &&
		    k60_touch_hw.gpio_irq == 21 &&
		    k60_touch_hw.gpio_avdd == 71, "gpios decoded");
		CHECK(k60_touch_hw.panel_max_x == 14400 &&
		    k60_touch_hw.panel_max_y == 32000, "axes decoded");
		CHECK(k60_touch_hw.spi_hz == 15000000, "spi ceiling decoded");
	} else if (strcmp(scenario, "dt_missing") == 0) {
		k60_touch_init();
		CHECK(k60_touch_hw.from_dt == 0, "falls back to the compiled wiring");
		CHECK(log_has("dt: no /k60-touch node"), "says the node is absent");
		CHECK(k60_touch_hw.se_base == 0x990000 &&
		    k60_touch_hw.gpio_irq == 21, "compiled defaults unchanged");
	} else if (strcmp(scenario, "rpmhoff") == 0) {
		build_cmd_db("ldoc2", 0x40004u, 1);
		fake_clocks(1);
		build_berlin_chip();
		boot_arg("k60touchstage", 4);
		boot_arg("k60rpmhoff", 1);
		k60_touch_init();
		run_thread();
		CHECK(log_has("iovdd vote skipped (k60rpmhoff=1), RSC not mapped"),
		    "says the vote is skipped and RSC stays unmapped");
		CHECK(rsc_maps == 0, "RSC is never mapped (%d maps)", rsc_maps);
		CHECK(rsc_triggers == 0, "no RSC trigger");
		CHECK(log_has("clocks ready"), "continues into the clock stage");
		CHECK(k60_touch_goodix_abi() == 1, "reaches the ID read");
		CHECK(log_has("K60 TOUCH: ID: abi=berlin"), "prints the ID summary");
	} else if (strcmp(scenario, "rscprobe_refused") == 0) {
		boot_arg("k60rpmhoff", 1);
		boot_arg("k60rscprobe", 1);
		k60_touch_init();
		run_thread();
		CHECK(log_has("rsc probe refused"), "refuses the probe with the bypass on");
		CHECK(rsc_maps == 0, "and never maps RSC (%d)", rsc_maps);
	} else if (strcmp(scenario, "id_readback") == 0) {
		build_touch_node();
		fake_clocks(1);
		build_berlin_chip();
		build_berlin_report(0x80, 1, 7200, 16000);
		boot_arg("k60touchstage", 1);
		boot_arg("k60touchid", 1);
		boot_arg("k60rpmhoff", 1);
		k60_touch_init();
		CHECK(log_has("init: stage 4"), "k60touchid raises the stage to 4");
		run_thread();
		CHECK(log_has("K60 TOUCH: ID: abi=berlin chip=\"911\""),
		    "prints the machine-readable ID line");
		CHECK(log_has("K60 TOUCH: ID: raw 0x10014 = 39 31 31 00"),
		    "prints the raw ID bytes");
		CHECK(log_has("k60touchid=1, ID readback only"),
		    "stops after the readback");
	} else {
		fprintf(stderr, "unknown scenario '%s'\n", scenario);
		return 2;
	}

	CHECK(unmapped_touched == 0, "no access outside the mapped windows (%d)",
	    unmapped_touched);
	if (failures != 0) {
		fprintf(stderr, "---- captured kernel log ----\n%s\n", logbuf);
		fprintf(stderr, "FAILED (%d)\n", failures);
		return 1;
	}
	fprintf(stderr, "PASS %s\n", scenario);
	return 0;
}
