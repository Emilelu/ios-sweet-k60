/*
 * logtail.c - host-side self-test for the sweet_log_dump() backward tail reader.
 *
 * This is a standalone harness: it does NOT include any kernel header and it
 * does not build the kernel. It contains
 *
 *   sweet_log_dump_new()  - the algorithm from
 *                           xnu/patches/0186-k60-gate-log.patch (hunk at
 *                           bsd/dev/sweet_ufs.c:915/926), copied verbatim,
 *                           with only `ufs_read_blocks()` replaced by a stub
 *                           that reads one 4096-byte block out of a simulated
 *                           partition image;
 *   sweet_log_dump_old()  - the implementation the patch replaces (from
 *                           xnu/patches/0152-k60-log-dump.patch), also verbatim,
 *                           used as the "last 48 lines" oracle and as the
 *                           positive control for the side-effect test.
 *
 * The only edits to the copied text are the ones marked "HARNESS:" (the ring
 * variable is a pointer instead of a static array, printf is redirected to a
 * capture buffer) plus the sabotage hooks, which are inert unless the test is
 * started with --sabotage=... on purpose.
 *
 * Everything the tests assert is derived independently:
 *   - the expected payload is cut out of the ground-truth byte stream that was
 *     used to synthesise the partition image (never out of the ring);
 *   - the "last 48 lines" expectation is computed forward from that stream;
 *   - the start of the printed region is checked to be a 4096-byte block
 *     boundary in ring-offset space (a structural property of the algorithm,
 *     independent of the payload comparison).
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* constants, as in bsd/dev/sweet_ufs.c                               */
/* ------------------------------------------------------------------ */

#define UFS_BLOCK        4096u
#define BOUNCE_SIZE      (1u << 20)
#define DMA_BOUNCE       64u
#define SWEET_LOG_SIZE   (4u << 20)
#define SWEET_LOG_BLOCKS (1u + SWEET_LOG_SIZE / UFS_BLOCK)

#ifndef MIN
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#endif

/* ------------------------------------------------------------------ */
/* console capture: the kernel's printf(), recorded instead of printed */
/* ------------------------------------------------------------------ */

/* The new dump can never print more than 16 KiB, but the OLD implementation
 * walks back 48 newlines and those 48 lines may be megabytes long, so the
 * capture buffer has to hold the whole ring (4 MiB) plus the headers. */
#define OUT_CAP (5u << 20)
static char   g_out[OUT_CAP];
static size_t g_out_len;
static int    g_out_ovf;

static int
cap_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (g_out_len + 1 >= OUT_CAP) {
		g_out_ovf = 1;
		return -1;
	}
	va_start(ap, fmt);
	n = vsnprintf(g_out + g_out_len, OUT_CAP - g_out_len, fmt, ap);
	va_end(ap);
	if (n > 0) {
		g_out_len += (size_t)n;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* sabotage hooks: inert unless --sabotage=<name> is given on purpose  */
/* ------------------------------------------------------------------ */

enum {
	SAB_NONE = 0,
	SAB_BOFF,       /* clamp to boff instead of boff + 1   (off by one)    */
	SAB_CLAMP,      /* read from the block start instead of clamping n     */
	SAB_WRAP,       /* drop the % SWEET_LOG_SIZE on idx    (no wrap)       */
	SAB_WRITE,      /* restore the old memcpy back into sweet_log_ring     */
	SAB_LINES,      /* ignore the 48-line limit                            */
	SAB_DST,        /* place a chunk one byte late in tail (gets bytes wrong) */
};
static int g_sab;

/* ------------------------------------------------------------------ */
/* simulated partition + the one block read primitive                  */
/* ------------------------------------------------------------------ */

static struct {
	uint64_t       first_block;
	uint64_t       nblocks;
	unsigned char *dma;
	int            writable;
} su;

static unsigned char *g_image;        /* whole simulated partition       */
static uint64_t       g_image_blocks;
static unsigned long  g_reads;        /* ufs_read_blocks() call count    */
static int            g_read_err;

static uint64_t
log_base(void)
{
	/* base = su.first_block + su.nblocks - SWEET_LOG_BLOCKS */
	return su.first_block + su.nblocks - SWEET_LOG_BLOCKS;
}

static int
ufs_read_blocks(uint64_t lba, uint32_t n)
{
	if (n == 0 || lba + n > g_image_blocks) {
		g_read_err = 1;
		return EIO;
	}
	g_reads++;
	memcpy(su.dma + DMA_BOUNCE, g_image + lba * UFS_BLOCK, (size_t)n * UFS_BLOCK);
	return 0;
}

/* same on-disk header layout as sweet_ufs.c */
struct sweet_log_header {
	char     magic[8];
	uint32_t version;
	uint32_t size;
	uint64_t total;
	uint64_t seq;
};

/* HARNESS: the kernel has `static char sweet_log_ring[SWEET_LOG_SIZE]`; here it
 * is a page-aligned allocation so that the write-protect probe can mprotect()
 * exactly this object and nothing else. */
static char    *sweet_log_ring;
static uint64_t sweet_log_total;

static int
PE_parse_boot_argn(const char *arg, void *val, int max)
{
	/* no such boot-arg in the harness: leaves the caller's default (on = 1) */
	(void)arg;
	(void)val;
	(void)max;
	return 0;
}

/* ================================================================== */
/* 0186-k60-gate-log.patch, bsd/dev/sweet_ufs.c @@ -915,40 +926,51 @@  */
/* ================================================================== */

#define printf cap_printf

static void
sweet_log_dump_new(void)
{
	uint64_t base = su.first_block + su.nblocks - SWEET_LOG_BLOCKS;
	struct sweet_log_header h;
	void *bounce = (void *)(su.dma + DMA_BOUNCE);
	int on = 1;
	int error;

	PE_parse_boot_argn("sweetlogdump", &on, sizeof(on));
	if (!on || !su.writable || su.nblocks < 2 * SWEET_LOG_BLOCKS) {
		return;
	}
	error = ufs_read_blocks(base, 1);
	if (error) {
		return;
	}
	memcpy(&h, bounce, sizeof(h));
	if (memcmp(h.magic, "SWEETLOG", 8) != 0 || h.version != 1 || h.size != SWEET_LOG_SIZE || h.total == 0) {
		return;
	}

	/*
	 * SWEET: walk the ring backwards through a private tail buffer instead of
	 * staging the whole ring in sweet_log_ring. The old code memcpy'd the
	 * previous boot's bytes back into sweet_log_ring at the same offsets this
	 * boot had already written to, so it silently replaced this boot's own
	 * early console - vm_page_bootstrap and the 0172 cap line included. The
	 * ring is read-only from here.
	 */
	static char tail[16 * 1024];
	uint32_t cap = (uint32_t)sizeof(tail);
	uint32_t have = 0, lines = 0;
	uint64_t want = MIN(h.total, (uint64_t)SWEET_LOG_SIZE);
	uint64_t first = (h.total - want) % SWEET_LOG_SIZE;
	uint64_t pos = want;

	while (pos > 0 && lines < 48 && have < cap) {
		/* SABOTAGE HOOK: drop the modulo (no ring wrap). */
		uint64_t idx = (g_sab == SAB_WRAP) ? (first + pos - 1) : ((first + pos - 1) % SWEET_LOG_SIZE);
		uint32_t blk = (uint32_t)(idx / UFS_BLOCK);
		uint32_t boff = (uint32_t)(idx % UFS_BLOCK);
		uint32_t n = (uint32_t)MIN((uint64_t)(cap - have), (uint64_t)UFS_BLOCK);
		int crosses = (n > boff + 1);

		/* never cross the start of the block we are about to read */
		/* SABOTAGE HOOK: SAB_BOFF clamps one byte short of it. */
		if (crosses && g_sab != SAB_CLAMP) {
			n = (g_sab == SAB_BOFF) ? ((boff > 0) ? boff : 1) : boff + 1;
		}
		error = ufs_read_blocks(base + 1 + blk, 1);
		if (error) {
			return;
		}
		/* SABOTAGE HOOK: SAB_CLAMP starts the chunk at the block start
		 * instead of shortening it, i.e. it re-copies the head of the
		 * block - the old implementation's behaviour. */
		uint32_t src = (g_sab == SAB_CLAMP && crosses) ? 0 : (uint32_t)(boff + 1 - n);
		/* SABOTAGE HOOK: SAB_DST shifts the destination by one byte. */
		size_t dst = (size_t)(cap - have - n);
		if (g_sab == SAB_DST && have > 0) {
			dst += 1;
		}
		memmove(tail + dst, (const char *)bounce + src, n);
		have += n;
		pos -= n;
		for (uint32_t i = 0; i < n; i++) {
			if (tail[cap - have + i] == '\n') {
				lines++;
			}
		}
		/* SABOTAGE HOOK: SAB_LINES ignores the 48-line limit. */
		if (g_sab == SAB_LINES) {
			lines = 0;
		}
	}
	if (have == 0) {
		return;
	}
	/* SABOTAGE HOOK: SAB_WRITE reintroduces the ring write. */
	if (g_sab == SAB_WRITE) {
		memcpy(sweet_log_ring, tail, cap);
	}

	printf("SWEET LOG: previous boot, last %u bytes (seq %llu)\n", have, h.seq);
	for (uint32_t i = cap - have; i < cap; i++) {
		char c = tail[i];

		if (c == '\n') {
			printf("\n");
		} else if (c >= 0x20 && c < 0x7f) {
			printf("%c", c);
		}
	}
	printf("\nSWEET LOG: end of previous boot log\n");
}

/* ================================================================== */
/* 0152-k60-log-dump.patch: the implementation being replaced          */
/* ================================================================== */

static void
sweet_log_dump_old(void)
{
	uint64_t base = su.first_block + su.nblocks - SWEET_LOG_BLOCKS;
	struct sweet_log_header h;
	void *bounce = (void *)(su.dma + DMA_BOUNCE);
	int on = 1;
	int error;

	PE_parse_boot_argn("sweetlogdump", &on, sizeof(on));
	if (!on || !su.writable || su.nblocks < 2 * SWEET_LOG_BLOCKS) {
		return;
	}
	error = ufs_read_blocks(base, 1);
	if (error) {
		return;
	}
	memcpy(&h, bounce, sizeof(h));
	if (memcmp(h.magic, "SWEETLOG", 8) != 0 || h.version != 1 || h.size != SWEET_LOG_SIZE || h.total == 0) {
		return;
	}

	/* Pull the ring in bounce-sized chunks into the (idle) static ring. */
	uint64_t got = 0;
	uint64_t want = MIN(h.total, (uint64_t)SWEET_LOG_SIZE);
	uint64_t first = (h.total - want) % SWEET_LOG_SIZE;

	while (got < want && !error) {
		uint64_t pos = (first + got) % SWEET_LOG_SIZE;
		uint32_t blk = (uint32_t)(pos / UFS_BLOCK);
		uint32_t n = (uint32_t)MIN((uint64_t)(SWEET_LOG_SIZE / UFS_BLOCK - blk),
		    (uint64_t)(BOUNCE_SIZE / UFS_BLOCK));

		error = ufs_read_blocks(base + 1 + blk, n);
		if (!error) {
			memcpy(sweet_log_ring + (uint64_t)blk * UFS_BLOCK, bounce, n * UFS_BLOCK);
			got += (uint64_t)n * UFS_BLOCK;
		}
	}
	if (error) {
		return;
	}

	/* Print only the last ~48 lines: the console is the delivery channel. */
	uint32_t end = (uint32_t)want;
	uint32_t lines = 0, start = end;

	while (start > 0 && lines < 48) {
		if (sweet_log_ring[(first + start - 1) % SWEET_LOG_SIZE] == '\n') {
			lines++;
		}
		start--;
	}
	printf("SWEET LOG: previous boot, last %u bytes (seq %llu)\n", end - start, h.seq);
	for (uint32_t i = start; i < end; i++) {
		char c = sweet_log_ring[(first + i) % SWEET_LOG_SIZE];

		if (c == '\n') {
			printf("\n");
		} else if (c >= 0x20 && c < 0x7f) {
			printf("%c", c);
		}
	}
	printf("\nSWEET LOG: end of previous boot log\n");
}

#undef printf

/* ------------------------------------------------------------------ */
/* ground truth builder                                               */
/* ------------------------------------------------------------------ */

#define CONTENT_CAP (16u << 20)
static unsigned char g_content[CONTENT_CAP];
static size_t        g_clen;
static size_t        g_line_idx;

static void
b_emit_line(size_t len)
{
	size_t start = g_clen;
	char tag[24];
	int t;

	if (start + len > CONTENT_CAP) {
		fprintf(stdout, "harness: content overflow\n");
		exit(2);
	}
	for (size_t i = 0; i < len; i++) {
		g_content[start + i] = (unsigned char)('a' + (int)((g_line_idx * 7 + i * 13) % 26));
	}
	t = snprintf(tag, sizeof(tag), "L%05zu:", g_line_idx);
	if (t > 0 && (size_t)t < len) {
		memcpy(g_content + start, tag, (size_t)t);
	}
	g_content[start + len - 1] = '\n';
	g_clen += len;
	g_line_idx++;
}

static void
b_lines(size_t count, size_t len)
{
	for (size_t i = 0; i < count; i++) {
		b_emit_line(len);
	}
}

static void
b_blob(unsigned char c, size_t len)
{
	if (g_clen + len > CONTENT_CAP) {
		fprintf(stdout, "harness: content overflow\n");
		exit(2);
	}
	memset(g_content + g_clen, c, len);
	g_clen += len;
}

static void
b_text(const char *s)
{
	size_t n = strlen(s);

	if (g_clen + n > CONTENT_CAP) {
		fprintf(stdout, "harness: content overflow\n");
		exit(2);
	}
	memcpy(g_content + g_clen, s, n);
	g_clen += n;
}

/* total bytes exactly: total/len lines of `len` bytes, then a newline-free blob */
static void
b_fixed_total(uint64_t total, size_t line_len)
{
	uint64_t lines = total / line_len;
	size_t rest = (size_t)(total % line_len);

	g_line_idx = 0;
	for (uint64_t i = 0; i < lines; i++) {
		b_emit_line(line_len);
	}
	if (rest) {
		b_blob('x', rest);
	}
}

/* ------------------------------------------------------------------ */
/* simulated partition image                                          */
/* ------------------------------------------------------------------ */

static const uint64_t CASE_SEQ = 4242;

static void
img_build(uint64_t total)
{
	struct sweet_log_header h;
	unsigned char *ring;

	memset(g_image, 0, (size_t)g_image_blocks * UFS_BLOCK);
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, "SWEETLOG", 8);
	h.version = 1;
	h.size = SWEET_LOG_SIZE;
	h.total = total;
	h.seq = CASE_SEQ;
	memcpy(g_image + log_base() * UFS_BLOCK, &h, sizeof(h));

	ring = g_image + (log_base() + 1) * UFS_BLOCK;
	if (total >= SWEET_LOG_SIZE) {
		/* ring offset r holds the newest content byte whose absolute index
		 * is congruent to r: k = total - 1 - ((total - 1 - r) % SIZE) */
		for (uint64_t r = 0; r < SWEET_LOG_SIZE; r++) {
			uint64_t k = total - 1 - ((total - 1 - r) % SWEET_LOG_SIZE);
			ring[r] = g_content[k];
		}
	} else {
		memcpy(ring, g_content, (size_t)total);
		/* stale bytes past the write pointer: reading these would be a bug */
		memset(ring + total, '#', SWEET_LOG_SIZE - (size_t)total);
	}
}

static char g_ring_save[SWEET_LOG_SIZE];

/* ring as the kernel would find it: [0,written) = this boot's console output,
 * [written,SIZE) = what the previous boot left there. */
static void
ring_build(size_t written)
{
	unsigned char *stale = g_image + (log_base() + 1) * UFS_BLOCK;

	sweet_log_total = written;
	for (size_t r = 0; r < SWEET_LOG_SIZE; r++) {
		sweet_log_ring[r] = (r < written) ? (char)('Z' - (char)(r % 7)) : (char)stale[r];
	}
	memcpy(g_ring_save, sweet_log_ring, SWEET_LOG_SIZE);
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static size_t
count_diff(const char *a, const char *b, size_t n)
{
	size_t d = 0;

	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			d++;
		}
	}
	return d;
}

static size_t
count_nl(const unsigned char *p, size_t n)
{
	size_t c = 0;

	for (size_t i = 0; i < n; i++) {
		if (p[i] == '\n') {
			c++;
		}
	}
	return c;
}

/* the kernel's console filter: '\n' and 0x20..0x7e survive, all else is dropped */
static size_t
filter_bytes(const unsigned char *src, size_t len, char *dst)
{
	size_t o = 0;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = src[i];

		if (c == '\n' || (c >= 0x20 && c < 0x7f)) {
			dst[o++] = (char)c;
		}
	}
	return o;
}

struct result {
	unsigned long      have;    /* "last %u bytes" as printed */
	unsigned long long seq;
	size_t             off;     /* payload inside g_out       */
	size_t             len;
};

/* The dump prints: header "\n", payload, then "SWEET LOG: end of previous boot
 * log\n" preceded by one separator '\n' that is not part of the payload. */
static const char TAIL_MARK[] = "SWEET LOG: end of previous boot log\n";

static int
parse_dump(struct result *r)
{
	size_t tl = sizeof(TAIL_MARK) - 1;
	size_t body_end;
	int skip = 0;
	unsigned long have = 0;
	unsigned long long seq = 0;

	memset(r, 0, sizeof(*r));
	if (g_out_len < tl + 1 || memcmp(g_out + g_out_len - tl, TAIL_MARK, tl) != 0) {
		return -1;
	}
	body_end = g_out_len - tl - 1;      /* drop the separator newline */
	if (g_out[body_end] != '\n') {
		return -4;
	}
	if (sscanf(g_out, "SWEET LOG: previous boot, last %lu bytes (seq %llu)%n",
	    &have, &seq, &skip) != 2 || skip <= 0) {
		return -2;
	}
	/* sscanf's %n lands on the newline that terminates the header line */
	if ((size_t)skip >= body_end || g_out[skip] != '\n') {
		return -3;
	}
	r->have = have;
	r->seq = seq;
	r->off = (size_t)skip + 1;
	r->len = body_end - r->off;
	return 0;
}

/* offset of the 48th '\n' counted backwards inside [start,end), or start */
static size_t
last48_start(const unsigned char *win, size_t wlen)
{
	size_t lines = 0;
	size_t p = wlen;

	while (p > 0 && lines < 48) {
		if (win[p - 1] == '\n') {
			lines++;
		}
		p--;
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* write-protect probe: prove the ring is never written               */
/* ------------------------------------------------------------------ */

static int
wp_probe(void)
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		if (mprotect(sweet_log_ring, SWEET_LOG_SIZE, PROT_READ) != 0) {
			_exit(90);
		}
		g_out_len = 0;
		g_out_ovf = 0;
		sweet_log_dump_new();
		_exit(0);
	}
	if (waitpid(pid, &st, 0) < 0) {
		return -2;
	}
	if (WIFSIGNALED(st)) {
		return -WTERMSIG(st);
	}
	if (!WIFEXITED(st)) {
		return -99;
	}
	if (WEXITSTATUS(st) != 0) {
		return -100 - WEXITSTATUS(st);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* one case                                                           */
/* ------------------------------------------------------------------ */

struct tcase {
	const char *name;
	uint64_t    total;      /* h.total / length of the ground-truth stream */
	long        expect_have;/* -1 = only structural checks                */
	long        expect_nl;  /* newlines inside the printed region, -1 = any */
	int         printable;  /* 1 = content has no bytes the filter drops   */
	void      (*build)(void);
};

static int         g_fail;
static const char *g_case = "?";
static int         g_quiet;

static void
failf(const char *fmt, ...)
{
	va_list ap;

	fprintf(stdout, "[FAIL] %-18s ", g_case);
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
	g_fail++;
}

static void
infof(const char *fmt, ...)
{
	va_list ap;

	if (g_quiet) {
		return;
	}
	fprintf(stdout, "       %-18s ", g_case);
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
}

static char g_expect[OUT_CAP];
static char g_newpay[OUT_CAP];      /* the new dump's payload, kept across runs */
static size_t g_newpay_len;

static void
first_diff_report(const char *what, const char *got, size_t glen,
    const char *want, size_t wlen)
{
	size_t n = MIN(glen, wlen);
	size_t i;

	for (i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			break;
		}
	}
	if (i == n && glen == wlen) {
		return;
	}
	if (i == n) {
		failf("%s: length mismatch, got %zu bytes, want %zu bytes", what, glen, wlen);
		return;
	}
	failf("%s: first mismatch at byte %zu: got 0x%02x, want 0x%02x "
	    "(got %zu bytes, want %zu)", what, i,
	    (unsigned char)got[i], (unsigned char)want[i], glen, wlen);
}

static void
run_case(const struct tcase *tc)
{
	struct result rn, ro;
	uint64_t want, first, start_ring;
	size_t written, changed, elen, wstart;
	size_t nl_new, nl_avail, p48, old_have;
	unsigned long reads_new;
	unsigned long olnl;
	int rc;

	g_case = tc->name;

	/* ---- ground truth ---- */
	g_clen = 0;
	g_line_idx = 0;
	tc->build();
	if (g_clen != tc->total) {
		failf("harness: builder produced %zu bytes, table says %llu",
		    g_clen, (unsigned long long)tc->total);
		return;
	}
	img_build(tc->total);

	written = tc->total < 200000 ? (size_t)tc->total : 200000;
	ring_build(written);

	/* ================= new implementation ================= */
	g_out_len = 0;
	g_out_ovf = 0;
	g_reads = 0;
	g_read_err = 0;
	sweet_log_dump_new();
	reads_new = g_reads;

	if (parse_dump(&rn) != 0) {
		failf("new dump produced no parsable output (reads=%lu read_err=%d "
		    "captured=%zu overflow=%d)", g_reads, g_read_err, g_out_len, g_out_ovf);
		return;
	}
	want = MIN(tc->total, (uint64_t)SWEET_LOG_SIZE);
	first = (tc->total - want) % SWEET_LOG_SIZE;

	if (rn.have == 0 || rn.have > 16384u) {
		failf("implausible byte count %lu (cap is 16384)", rn.have);
	}
	if (rn.have > want) {
		failf("printed %lu bytes but only %llu exist", rn.have, (unsigned long long)want);
	}
	if (rn.seq != CASE_SEQ) {
		failf("seq %llu, want %llu", rn.seq, (unsigned long long)CASE_SEQ);
	}
	if (rn.have > g_clen) {
		failf("printed %lu bytes but the stream is %zu", rn.have, g_clen);
		return;
	}

	/* (1) the printed bytes are exactly the true tail of the stream */
	wstart = g_clen - (size_t)rn.have;
	elen = filter_bytes(g_content + wstart, (size_t)rn.have, g_expect);
	first_diff_report("payload is not the true tail", g_out + rn.off, rn.len, g_expect, elen);
	memcpy(g_newpay, g_out + rn.off, rn.len);
	g_newpay_len = rn.len;

	if (tc->printable && rn.len != rn.have) {
		failf("all-printable stream but payload is %zu of %lu bytes", rn.len, rn.have);
	}
	if (!tc->printable && rn.len >= rn.have) {
		failf("filter dropped nothing on a stream with control bytes (%zu of %lu)",
		    rn.len, rn.have);
	}

	/* (2) number of lines actually delivered */
	nl_new = count_nl(g_content + wstart, (size_t)rn.have);
	nl_avail = count_nl(g_content + (g_clen - (size_t)want), (size_t)want);
	if (rn.have == 16384u) {
		infof("16 KiB cap hit: %zu byte(s) printed, %zu newline(s)", rn.len, nl_new);
	} else if (nl_new < 48 && nl_avail >= 48) {
		failf("only %zu newline(s) in %lu bytes although %zu are reachable and "
		    "the cap was not hit", nl_new, rn.have, nl_avail);
	}
	if (tc->expect_nl >= 0 && nl_new != (size_t)tc->expect_nl) {
		failf("%zu newline(s) per the expectations, got %zu", (size_t)tc->expect_nl, nl_new);
	}
	if (tc->expect_have >= 0 && rn.have != (unsigned long)tc->expect_have) {
		failf("expected last %ld bytes, got %lu", tc->expect_have, rn.have);
	}

	/* (3) structural: the region must start on a 4096-byte boundary in ring
	 * space, unless it was clipped by the 16 KiB cap */
	if (rn.have < want && rn.have < 16384u) {
		start_ring = (first + want - rn.have) % SWEET_LOG_SIZE;
		if (start_ring % UFS_BLOCK != 0) {
			failf("printed region starts at ring offset 0x%llx, not a 4096 "
			    "boundary - chunk arithmetic is off",
			    (unsigned long long)start_ring);
		}
	}
	if (reads_new > 6) {
		failf("%lu block reads for at most 16 KiB - excessive", reads_new);
	}

	/* (4) side effect: nothing in the ring may change */
	changed = count_diff(sweet_log_ring, g_ring_save, written);
	if (changed != 0) {
		failf("new dump modified sweet_log_ring: %zu of %zu this-boot bytes "
		    "in [0,%zu) changed", changed, written, written);
	}
	changed = count_diff(sweet_log_ring, g_ring_save, SWEET_LOG_SIZE);
	if (changed != 0) {
		failf("new dump modified %zu byte(s) of the 4 MiB ring", changed);
	}

	/* (5) write-protect probe: any write to the ring kills the child */
	rc = wp_probe();
	if (rc != 0) {
		failf("write-protect probe: the dump wrote to sweet_log_ring "
		    "(child rc=%d, signal %d)", rc, rc < 0 ? -rc : 0);
	}

	/* ================= old implementation ================= */
	memcpy(sweet_log_ring, g_ring_save, SWEET_LOG_SIZE);
	g_out_len = 0;
	g_out_ovf = 0;
	g_reads = 0;
	g_read_err = 0;
	sweet_log_dump_old();

	if (parse_dump(&ro) != 0) {
		failf("old dump produced no parsable output (reads=%lu read_err=%d "
		    "captured=%zu overflow=%d)", g_reads, g_read_err, g_out_len, g_out_ovf);
		return;
	}

	/* independent "last 48 lines" expectation, cut from the stream */
	p48 = last48_start(g_content + (g_clen - (size_t)want), (size_t)want);
	old_have = (size_t)want - p48;
	elen = filter_bytes(g_content + (g_clen - (size_t)want) + p48, old_have, g_expect);
	first_diff_report("old dump is not the last-48-lines oracle",
	    g_out + ro.off, ro.len, g_expect, elen);
	if (ro.have != old_have) {
		failf("old dump says %lu bytes, oracle %zu", ro.have, old_have);
	}
	olnl = (unsigned long)count_nl(g_content + g_clen - old_have, old_have);
	if (old_have < (size_t)want && olnl != 48) {
		failf("old dump delivered %lu newlines, the oracle says 48", olnl);
	}
	/* The new region must be a superset (in the suffix sense) of the old one,
	 * except when the 16 KiB cap cut it short - that is a documented
	 * consequence of the cap, reported separately below. */
	if (old_have > rn.have) {
		if (rn.have == 16384u) {
			infof("16 KiB cap: new reader delivers %lu bytes / %zu lines, "
			    "the old reader delivered %zu / %lu", rn.have, nl_new,
			    old_have, (unsigned long)count_nl(g_content + g_clen - old_have, old_have));
		} else {
			failf("old region is %zu bytes, new only %lu - the new reader "
			    "loses lines the old one delivered", old_have, rn.have);
		}
	} else {
		size_t o = g_clen - old_have;
		elen = filter_bytes(g_content + o, old_have, g_expect);
		first_diff_report("new payload does not end with the old 48 lines",
		    g_newpay + g_newpay_len - elen, elen, g_expect, elen);
	}

	/* (6) the old code must visibly clobber this boot's console */
	changed = count_diff(sweet_log_ring, g_ring_save, written);
	if (changed == 0) {
		failf("old dump left the ring untouched - the side-effect test has no "
		    "discriminating power (written=%zu)", written);
	}

	if (g_quiet) {
		return;
	}
	fprintf(stdout, "[ ok ] %-18s total=%-8llu have=%-5lu printed=%-5zu nl=%-3zu "
	    "reads=%-2lu old=%zu bytes%s\n",
	    tc->name, (unsigned long long)tc->total, rn.have, rn.len, nl_new,
	    reads_new, old_have,
	    (rn.len && g_out[rn.off] == '\n') ? " (starts at a line boundary)" : "");
}

/* ------------------------------------------------------------------ */
/* the cases                                                          */
/* ------------------------------------------------------------------ */

static void c_small(void)        { b_fixed_total(3000, 100); }
static void c_exact_block(void)  { b_fixed_total(4096, 64); }
static void c_two_blocks(void)   { b_fixed_total(8192, 64); }
static void c_just_over(void)    { b_fixed_total(5000, 100); }
static void c_typical(void)      { b_fixed_total(10000, 100); }
static void c_ring_full(void)    { b_fixed_total(SWEET_LOG_SIZE, 100); }
static void c_near_full(void)    { b_fixed_total(SWEET_LOG_SIZE - 1, 100); }
static void c_wrap_777(void)     { b_fixed_total(6292233, 100); }
static void c_wrap_1000(void)    { b_fixed_total(5243880, 100); }
static void c_wrap_2laps(void)   { b_fixed_total(2 * (uint64_t)SWEET_LOG_SIZE, 100); }
static void c_wrap_aligned(void) { b_fixed_total(SWEET_LOG_SIZE + 4096, 100); }
static void c_nl_dense(void)     { b_fixed_total(16000, 16); }

/* The three below put h.total % SWEET_LOG_SIZE at 1, 777 and 4097, so the walk
 * starts at ring offset 0 / 776 / 4096 and therefore has to splice across
 * ring offset 0 into block 1023 - the wrap the other cases never reach,
 * because there `first` is far larger than the 16 KiB the reader may take. */
static void c_wrap_head1(void)   { b_fixed_total(3 * (uint64_t)SWEET_LOG_SIZE + 1, 100); }
static void c_wrap_3laps(void)   { b_fixed_total(3 * (uint64_t)SWEET_LOG_SIZE + 777, 100); }
static void c_wrap_blk1(void)    { b_fixed_total(3 * (uint64_t)SWEET_LOG_SIZE + 4097, 100); }

/* long but not gigantic lines: neither the 48-line nor the block geometry
 * lines up, the 16 KiB cap is what ends it with pos > 0 left over */
static void c_longlines(void)    { b_fixed_total(20000, 800); }

/* ---- randomised cases (--fuzz=N) ---------------------------------- */

static unsigned char g_stage[CONTENT_CAP];
static size_t        g_stage_len;

static void
c_fuzz(void)
{
	memcpy(g_content, g_stage, g_stage_len);
	g_clen = g_stage_len;
	g_line_idx = 0;
}

/* xorshift64*, so a failing seed reproduces exactly */
static uint64_t
rnd(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 2685821657736338717ull;
}

static void
fuzz_make(uint64_t seed)
{
	/* lengths that land on the interesting offsets: a whole number of laps
	 * plus 0/1/2/4095/4096/4097/8191/8192/16383/16384/16385 bytes, which is
	 * exactly where first (h.total % SIZE) is small or block aligned and the
	 * walk has to splice across ring offset 0, clamp to a block start, or hit
	 * the 16 KiB cap one byte early */
	static const uint64_t edge[] = {
		0, 1, 2, 31, 100, 4095, 4096, 4097, 8191, 8192, 16383, 16384, 16385,
	};
	static const uint64_t laps[] = { 1, 2, 3 };
	uint64_t s = seed ? seed : 1;
	uint64_t span = 3ull * SWEET_LOG_SIZE;
	size_t len = (size_t)(rnd(&s) % span) + 1;
	unsigned nl_pct = (unsigned)(rnd(&s) % 60) + 1;   /* 1%..60% newlines */
	unsigned long long run = 0;

	if ((rnd(&s) % 2) == 0) {
		len = (size_t)(laps[rnd(&s) % 3] * (uint64_t)SWEET_LOG_SIZE
		    + edge[rnd(&s) % (sizeof(edge) / sizeof(edge[0]))]);
		if (len == 0) {
			len = 1;
		}
	}

	for (size_t i = 0; i < len; i++) {
		/* occasionally start a long newline-free run, like a hex dump */
		if (run == 0 && (rnd(&s) % 97) == 0) {
			run = (rnd(&s) % 24000) + 1;
		}
		if (run > 0) {
			g_stage[i] = (unsigned char)('0' + (int)(rnd(&s) % 40));
			run--;
		} else if ((rnd(&s) % 100) < nl_pct) {
			g_stage[i] = '\n';
		} else {
			g_stage[i] = (unsigned char)(0x20 + (rnd(&s) % 0x5f));
		}
	}
	g_stage_len = len;
}

static void
c_bigline_end(void)
{
	g_line_idx = 0;
	b_lines(50, 100);
	b_blob('X', 40000);      /* one 40 KiB line with no newline */
}

static void
c_bigline_mid(void)
{
	g_line_idx = 0;
	b_lines(100, 100);
	b_blob('X', 20000);      /* 20 KiB line in the middle */
	b_lines(3, 100);
}

static void
c_noeol(void)
{
	g_line_idx = 0;
	b_lines(10, 100);
	b_text("NOEOLTAIL");
}

static void
c_ctrl(void)
{
	static const unsigned char nasty[] = { 0x07, 0x1b, 0x7f, 0x80, 0xff, 0x00 };

	g_line_idx = 0;
	b_lines(20, 100);
	for (size_t i = 0; i < 1000; i++) {
		char c = (char)nasty[i % sizeof(nasty)];
		b_blob((unsigned char)c, 1);
	}
}

static const struct tcase g_cases[] = {
	/* expect_have is worked out by hand from the geometry of each stream:
	 * the walk stops at 48 newlines, at the 16 KiB cap, or at pos == 0, and
	 * each chunk ends on a 4096-byte boundary of ring-offset space. */
	{ "small_3000",       3000,                    3000,  30,   1, c_small },
	{ "exact_block_4096", 4096,                    4096,  64,   1, c_exact_block },
	{ "two_blocks_8192",  8192,                    4096,  64,   1, c_two_blocks },
	{ "just_over_block",  5000,                    5000,  50,   1, c_just_over },
	{ "typical_10000",    10000,                   5904,  60,   1, c_typical },
	{ "no_eol_tail",      1009,                    1009,  10,   1, c_noeol },
	{ "control_bytes",    3000,                    3000,  20,   0, c_ctrl },
	{ "newline_dense",    16000,                   3712,  232,  1, c_nl_dense },
	{ "bigline_at_end",   45000,                   16384, 0,    1, c_bigline_end },
	{ "bigline_middle",   30300,                   16384, 3,    1, c_bigline_mid },
	{ "ring_exactly_full", SWEET_LOG_SIZE,         8192,  -1,   1, c_ring_full },
	{ "ring_minus_one",    SWEET_LOG_SIZE - 1,     8191,  -1,   1, c_near_full },
	{ "wrap_head_777",     6292233,                4873,  -1,   1, c_wrap_777 },
	{ "wrap_head_1000",    5243880,                5096,  -1,   1, c_wrap_1000 },
	{ "wrap_block_align",  SWEET_LOG_SIZE + 4096,  8192,  -1,   1, c_wrap_aligned },
	{ "wrap_two_laps",     2 * (uint64_t)SWEET_LOG_SIZE, 8192, -1, 1, c_wrap_2laps },
	{ "wrap_head_1byte",   3 * (uint64_t)SWEET_LOG_SIZE + 1, 8193, -1, 1, c_wrap_head1 },
	{ "wrap_head_777_3x",  3 * (uint64_t)SWEET_LOG_SIZE + 777, 4873, -1, 1, c_wrap_3laps },
	{ "wrap_head_blk_1b",  3 * (uint64_t)SWEET_LOG_SIZE + 4097, 8193, -1, 1, c_wrap_blk1 },
	{ "longlines_20000",   20000,                  16384, -1,   1, c_longlines },
};

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static const struct {
	const char *name;
	int         id;
} g_sabotages[] = {
	{ "boff",  SAB_BOFF },
	{ "clamp", SAB_CLAMP },
	{ "wrap",  SAB_WRAP },
	{ "write", SAB_WRITE },
	{ "lines", SAB_LINES },
	{ "dst",   SAB_DST },
};

static void
usage(const char *argv0)
{
	fprintf(stdout,
	    "usage: %s [--sabotage=NAME] [--list] [--case=NAME]\n"
	    "  verifies sweet_log_dump()'s backward tail reader on the host\n"
	    "  --sabotage=NAME  inject a known bug and expect failures:\n", argv0);
	for (size_t i = 0; i < sizeof(g_sabotages) / sizeof(g_sabotages[0]); i++) {
		fprintf(stdout, "                   %s\n", g_sabotages[i].name);
	}
	fprintf(stdout, "  --list           list the cases\n");
	fprintf(stdout, "  --case=NAME      run a single case\n");
}

int
main(int argc, char **argv)
{
	const char *only = NULL;
	int list = 0;
	char *p;
	size_t i;
	long fuzz = 0;

	for (int a = 1; a < argc; a++) {
		if (strcmp(argv[a], "--list") == 0) {
			list = 1;
		} else if (strncmp(argv[a], "--case=", 7) == 0) {
			only = argv[a] + 7;
		} else if (strncmp(argv[a], "--fuzz=", 7) == 0) {
			fuzz = strtol(argv[a] + 7, &p, 10);
			if (*p != '\0' || fuzz < 0 || fuzz > 100000) {
				fprintf(stdout, "bad --fuzz value\n");
				return 2;
			}
		} else if (strncmp(argv[a], "--sabotage=", 11) == 0) {
			p = argv[a] + 11;
			for (i = 0; i < sizeof(g_sabotages) / sizeof(g_sabotages[0]); i++) {
				if (strcmp(p, g_sabotages[i].name) == 0) {
					g_sab = g_sabotages[i].id;
					break;
				}
			}
			if (i == sizeof(g_sabotages) / sizeof(g_sabotages[0])) {
				fprintf(stdout, "unknown sabotage \"%s\"\n", p);
				return 2;
			}
		} else if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stdout, "unknown argument \"%s\"\n", argv[a]);
			usage(argv[0]);
			return 2;
		}
	}
	if (list) {
		for (i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++) {
			fprintf(stdout, "%-18s total=%llu\n", g_cases[i].name,
			    (unsigned long long)g_cases[i].total);
		}
		return 0;
	}

	/* 2 * SWEET_LOG_BLOCKS so the dump's own guard lets it run */
	su.writable = 1;
	su.nblocks = 2 * SWEET_LOG_BLOCKS;
	su.first_block = 1000;
	su.dma = calloc(1, DMA_BOUNCE + BOUNCE_SIZE);
	g_image_blocks = su.first_block + su.nblocks + 16;
	g_image = calloc(1, (size_t)g_image_blocks * UFS_BLOCK);
	if (!su.dma || !g_image) {
		fprintf(stdout, "out of memory\n");
		return 2;
	}
	if (posix_memalign((void **)&sweet_log_ring, 4096, SWEET_LOG_SIZE) != 0) {
		fprintf(stdout, "out of memory\n");
		return 2;
	}
	memset(sweet_log_ring, 0, SWEET_LOG_SIZE);

	fprintf(stdout, "logtail: %s\n",
	    g_sab ? "SABOTAGED RUN - failures are expected" : "verifying sweet_log_dump()");
	fprintf(stdout, "  ring 0x%zx bytes, cap 16384, 48 lines, image base block %llu\n",
	    (size_t)SWEET_LOG_SIZE, (unsigned long long)log_base());

	for (i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++) {
		if (only && strcmp(only, g_cases[i].name) != 0) {
			continue;
		}
		run_case(&g_cases[i]);
	}

	if (fuzz > 0 && !only) {
		int before = g_fail;

		fprintf(stdout, "\n[fuzz] %ld randomised streams (totals up to 12 MiB, "
		    "newline density 1%%..60%%, planted newline-free runs)\n", fuzz);
		for (long k = 0; k < fuzz; k++) {
			struct tcase tc;
			char namebuf[32];
			uint64_t seed = 7919ull * (uint64_t)(k + 1) + 1000;

			fuzz_make(seed);
			snprintf(namebuf, sizeof(namebuf), "fuzz#%ld", k);
			tc.name = namebuf;
			tc.total = g_stage_len;
			tc.expect_have = -1;
			tc.expect_nl = -1;
			tc.printable = 1;
			for (size_t j = 0; j < g_stage_len; j++) {
				unsigned char c = g_stage[j];

				if (c != '\n' && (c < 0x20 || c >= 0x7f)) {
					tc.printable = 0;
					break;
				}
			}
			tc.build = c_fuzz;
			g_quiet = 1;
			run_case(&tc);
			g_quiet = 0;
			if (g_fail != before) {
				fprintf(stdout, "       ^ reproducer: --fuzz=%ld + the "
				    "generator seed %llu (%zu bytes)\n", k + 1,
				    (unsigned long long)seed, g_stage_len);
				before = g_fail;
			}
		}
		if (g_fail == 0) {
			fprintf(stdout, "[ ok ] fuzz              %ld/%ld streams matched "
			    "the true tail\n", fuzz, fuzz);
		}
	}

	fprintf(stdout, "\n%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
	return g_fail ? 1 : 0;
}
