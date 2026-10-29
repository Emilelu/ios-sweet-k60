#pragma once
/*
 * 最小替身：只够 bsd/dev/k60_touch_dt.c 编译与跑宿主冒烟台。
 * 真内核里这两个函数来自 pexpert/device_tree.h，签名保持不变。
 */
#include <sys/types.h>

typedef void *DTEntry;
typedef int DTStatus;

#ifndef kSuccess
#define kSuccess 0
#endif

DTStatus SecureDTLookupEntry(const DTEntry searchPoint, const char *pathName,
    DTEntry *foundEntry);
DTStatus SecureDTGetProperty(DTEntry entry, const char *propertyName,
    void const **propertyValue, unsigned int *propertySize);
