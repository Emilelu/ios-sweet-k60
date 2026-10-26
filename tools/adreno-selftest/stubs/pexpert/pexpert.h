#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
#ifdef __cplusplus
}
#endif
