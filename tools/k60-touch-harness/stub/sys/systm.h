#pragma once
#include <sys/types.h>
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void delay(int usec);
