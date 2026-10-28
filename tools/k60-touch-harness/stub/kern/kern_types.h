#pragma once
#include <sys/types.h>
typedef struct thread *thread_t;
typedef int wait_result_t;
typedef void *event_t;
typedef int wait_interrupt_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL ((thread_continue_t)0)
#define KERN_SUCCESS 0
