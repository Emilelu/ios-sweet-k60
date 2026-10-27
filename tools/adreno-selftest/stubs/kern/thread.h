#pragma once
/* 仿真空壳：只提供 0190 用到的线程入口类型与调用，实现见 host.cpp */
#ifdef __cplusplus
extern "C" {
#endif
typedef void *thread_t;
typedef void (*thread_continue_t)(void *, int);
typedef int wait_result_t;
int kernel_thread_start(thread_continue_t func, void *arg, thread_t *thread);
void thread_deallocate(thread_t thread);
#ifdef __cplusplus
}
#endif
