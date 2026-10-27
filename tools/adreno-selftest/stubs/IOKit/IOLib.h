#pragma once
/* 仿真空壳：IOLog 在 IOService.h 里声明 */
#include <IOKit/IOService.h>
/* 内核里是 C++ 符号，仿真台为了过链接守卫用 C 名字，实现见 host.cpp */
#ifdef __cplusplus
extern "C" {
#endif
void IOSleep(unsigned int milliseconds);
#ifdef __cplusplus
}
#endif
