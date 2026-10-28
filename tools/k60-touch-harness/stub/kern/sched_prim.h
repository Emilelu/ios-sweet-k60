#pragma once
#include <kern/kern_types.h>
extern wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t interruptible, uint64_t deadline);
extern wait_result_t thread_block(thread_continue_t continuation);
