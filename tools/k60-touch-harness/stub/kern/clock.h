#pragma once
#include <sys/types.h>
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
uint64_t mach_absolute_time(void);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *result);
void delay_for_interval(uint32_t interval, uint32_t scale);
