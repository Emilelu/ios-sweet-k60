#pragma once
/* 最小内核类型替身：64/32 位整数直接用宿主 glibc 的，避免 int64_t 冲突。 */
#include <stdint.h>
#include <stddef.h>
typedef unsigned long vm_offset_t;
typedef unsigned long vm_size_t;
typedef int kern_return_t;
typedef _Bool boolean_t;
typedef _Bool bool;
#ifndef true
#define true 1
#define false 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#define TRUE 1
#define FALSE 0
