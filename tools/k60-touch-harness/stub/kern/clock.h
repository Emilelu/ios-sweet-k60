#pragma once
#include <sys/types.h>
#define NSEC_PER_MSEC 1000000ull
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *result);
