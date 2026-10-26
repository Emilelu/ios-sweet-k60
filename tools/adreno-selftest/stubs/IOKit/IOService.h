/* 仿真用的最小 IOKit 表面：只保留 SweetAdreno.cpp 实际用到的东西。 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pexpert/pexpert.h>

typedef unsigned long vm_offset_t;
typedef unsigned long vm_size_t;

#ifndef APPLE_KEXT_OVERRIDE
#define APPLE_KEXT_OVERRIDE
#endif
#ifndef __BEGIN_DECLS
#define __BEGIN_DECLS
#endif
#ifndef __END_DECLS
#define __END_DECLS
#endif

extern "C" void IOLog(const char *fmt, ...);

class OSObject {
public:
	virtual ~OSObject() {}
};

class IOService : public OSObject {
public:
	virtual bool start(IOService *provider) { (void)provider; return true; }
	void registerService() {}
};

#define OSDeclareDefaultStructors(cls) \
public: \
	cls(); \
protected: \
	cls(const cls &copy)
#define OSDefineMetaClassAndStructors(cls, super) \
	cls::cls() {} \
	cls::cls(const cls &copy) { (void)copy; }
