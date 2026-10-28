#pragma once
#include <sys/types.h>
#include <string.h>
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void delay(int usec);
