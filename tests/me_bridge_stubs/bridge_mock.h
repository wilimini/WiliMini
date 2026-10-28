#ifndef WILIMINI_BRIDGE_MOCK_H
#define WILIMINI_BRIDGE_MOCK_H
#include <stdint.h>
#define PSP_MODULE_INFO(...)
#define PSP_NO_CREATE_MAIN_THREAD()
#define PSP_MEMORY_PARTITION_USER 2
#define ME_CORE_T2_IMG_TABLE 2
#define ME_CORE_IMG_TABLE 3
#define DMACPLUS_BYTE_COUNT_4 2
typedef unsigned SceSize;
typedef int (*FPCall)(void *);
typedef int (*FCall)(void);
typedef struct { unsigned *mem; int uid; } Aligned64;
typedef struct { unsigned index, addr; } MistInjector;
typedef struct { unsigned index; void *param; } MistTrigger;
unsigned *test_register(unsigned address);
#define hw(address) (*test_register(address))
int pspSdkSetK1(int value);
int meCoreGetTableIdFromWitnessWord(void);
void sceKernelDcacheWritebackInvalidateAll(void);
void sceKernelDcacheWritebackInvalidateRange(void *, unsigned);
void sceKernelDcacheInvalidateRange(void *, unsigned);
void sceKernelDcacheWritebackRange(void *, unsigned);
Aligned64 meMistGetAligned64Mem(char *, int, unsigned);
void cleanChannels(void);
void clearPendingOperations(void);
void dmacplusFromMe(unsigned, unsigned, unsigned, unsigned);
unsigned sceKernelGetSystemTimeLow(void);
void sceKernelDelayThread(unsigned);
void sceKernelFreePartitionMemory(int);
void meKernelMistInjectSyscall(MistInjector *);
int meKernelMistTrigger(MistTrigger *);
#endif
