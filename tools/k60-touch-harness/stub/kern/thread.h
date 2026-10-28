#pragma once
#include <kern/kern_types.h>
thread_t current_thread(void);
kern_return_t kernel_thread_start(thread_continue_t cont, void *param, thread_t *out);
void thread_deallocate(thread_t t);
kern_return_t thread_terminate(thread_t t);
