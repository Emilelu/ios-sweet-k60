/*
 * x86 model of the arm64 page walk in osfmk/vm/vm_resident.c:pmap_startup(),
 * after 0170 (panel out of the free lists) and 0184 (notch table).
 *
 * It answers one question: given an ADT dram window and the kernel's notch
 * table, which physical pages can end up on a free list? Those are the pages
 * some later allocation will touch, so they must all be DRAM the AP can
 * decode. Everything else (carve-outs, gunyah, qtee) must be notched and
 * therefore never leave the vm_page array.
 *
 * The arithmetic is copied from the kernel, not invented:
 *
 *   mem_sz = pmap_free_pages_span() * PAGE;              // whole window
 *   npages = mem_sz / (PAGE + sizeof(*vm_pages));        // array capacity
 *   vm_pages = pmap_steal_freeable_memory(npages * sizeof);  // bumps first_avail
 *   for (i = 0; i < npages; i++) {
 *           if (!pmap_next_page(&phys_page)) break;      // first_avail == avail_end
 *           vm_page_init(&vm_pages[i], phys_page, FALSE);
 *           if (!SWEET_EXCLUDED_PAGE(&vm_pages[i])) vm_page_release_startup(...);
 *   }
 *
 * sizeof(struct vm_page) is 80 bytes for the 64-bit arm build with
 * VM_PAGE_PACKED_FROM_ARRAY; a few bytes either way moves npages by <0.5%,
 * which changes no verdict here (the tool prints its own numbers).
 *
 *   sim --start 0xb6900000 --end 0xe54f0000 [--notch 0xb8000000:0xb9200000 ...]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x4000UL
#define SIZEOF_VM_PAGE 80UL
#define MAX_NOTCH 16

struct notch {
	uint64_t lo, hi;
};

static struct notch notches[MAX_NOTCH];
static int nnotches;

static int notched(uint64_t pa)
{
	for (int i = 0; i < nnotches; i++)
		if (pa >= notches[i].lo && pa < notches[i].hi)
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t start = 0, end = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--start") && i + 1 < argc) {
			start = strtoull(argv[++i], 0, 0);
		} else if (!strcmp(argv[i], "--end") && i + 1 < argc) {
			end = strtoull(argv[++i], 0, 0);
		} else if (!strcmp(argv[i], "--notch") && i + 1 < argc) {
			char *s = argv[++i], *colon = strchr(s, ':');
			if (!colon || nnotches == MAX_NOTCH) {
				fprintf(stderr, "bad --notch %s\n", s);
				return 2;
			}
			*colon = 0;
			notches[nnotches].lo = strtoull(s, 0, 0);
			notches[nnotches].hi = strtoull(colon + 1, 0, 0);
			nnotches++;
		} else {
			fprintf(stderr, "usage: sim --start HEX [--end HEX] "
			    "[--notch LO:HI]...\n");
			return 2;
		}
	}
	if (!start || end <= start) {
		fprintf(stderr, "sim: need --start < --end\n");
		return 2;
	}
	if ((start | end) & (PAGE - 1)) {
		fprintf(stderr, "sim: window is not 16 KiB aligned\n");
		return 2;
	}
	for (int i = 0; i < nnotches; i++) {
		for (int j = i + 1; j < nnotches; j++) {
			if (notches[j].lo < notches[i].lo) {
				struct notch t = notches[i];
				notches[i] = notches[j];
				notches[j] = t;
			}
		}
		if (notches[i].hi <= notches[i].lo) {
			fprintf(stderr, "sim: empty notch\n");
			return 2;
		}
		if (i && notches[i].lo < notches[i - 1].hi) {
			fprintf(stderr, "sim: notches overlap\n");
			return 2;
		}
	}

	uint64_t span = end - start;
	uint64_t span_pages = span / PAGE;
	uint64_t npages = span / (PAGE + SIZEOF_VM_PAGE);
	uint64_t array_pages = (npages * SIZEOF_VM_PAGE + PAGE - 1) / PAGE;
	uint64_t first = start + array_pages * PAGE;
	uint64_t notched_pages = 0, free_pages = 0;

	/* pmap_steal_freeable_memory() lands on the bottom of the window: if a
	 * notch covered it, the array itself would be in firmware memory. */
	if (notched(first) || notched(first + array_pages * PAGE - 1)) {
		fprintf(stderr, "sim: vm_page array falls inside a notch\n");
		return 1;
	}

	for (uint64_t pa = first; pa < end; pa += PAGE) {
		if (notched(pa)) {
			notched_pages++;
		} else {
			free_pages++;
		}
	}
	printf("SPAN start=%#llx end=%#llx\n", (unsigned long long)start,
	    (unsigned long long)end);
	printf("STAT span_pages=%llu npages=%llu array_pages=%llu first=%#llx "
	    "last=%#llx notched=%llu free=%llu free_bytes=%llu\n",
	    (unsigned long long)span_pages, (unsigned long long)npages,
	    (unsigned long long)array_pages, (unsigned long long)first,
	    (unsigned long long)(end - PAGE), (unsigned long long)notched_pages,
	    (unsigned long long)free_pages, (unsigned long long)free_pages * PAGE);

	/* coalesce the free pages into byte runs */
	uint64_t run_lo = 0;
	int in_run = 0;
	for (uint64_t pa = first; pa < end; pa += PAGE) {
		int free_now = !notched(pa);
		if (free_now && !in_run) {
			run_lo = pa;
			in_run = 1;
		} else if (!free_now && in_run) {
			printf("RUN %#llx-%#llx\n", (unsigned long long)run_lo,
			    (unsigned long long)pa);
			in_run = 0;
		}
	}
	if (in_run)
		printf("RUN %#llx-%#llx\n", (unsigned long long)run_lo,
		    (unsigned long long)end);
	printf("END\n");
	return 0;
}
