/* Uses mcidclan's MIT-licensed ME MIST and core mapper, see third_party
 * revisions in docs/references.lock.json. No modifications to those sources. */
#include <pspkernel.h>
#include <pspsdk.h>
#include <psputilsforkernel.h>
#include <me-mist.h>
#include <me-mist-kcall.h>
#include <me-mist-utils.h>
#include <me-core-mapper/dmacplus.h>
#include "me_bridge.h"
#include "firmware_profile.h"
#include <stdio.h>

int wiliMeMistInit(unsigned restart_entry);

PSP_MODULE_INFO("Full ME Bridge", 0x1006, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();
static int ready;
static MeJob *pending;
static unsigned original_entry;

/* MIST user-mode entry points are unused in this kernel-only bridge. */
int meMistCall(FPCall f, void *p) { return f(p); }
int kCall_2(FCall f, unsigned int seg) { return ((FCall)((uintptr_t)f | seg))(); }
int kCall_3(FPCall f, unsigned int seg, void *p) { return ((FPCall)((uintptr_t)f | seg))(p); }

int meLabInit(uint32_t entry)
{
    int k1 = pspSdkSetK1(0);
    if (ready) { pspSdkSetK1(k1); return 0; }
    int table = meCoreGetTableIdFromWitnessWord();
    unsigned table_address = me_bridge_table_address(table);
    if (!table_address) { pspSdkSetK1(k1); return -100 - table; }
    unsigned firmware_entry = hw(0xbfc0071c);
    unsigned restart_entry = me_bridge_restart_entry(table, firmware_entry);
    printf("ME profile=%s table=0x%08x firmware_restart=0x%08x restart=0x%08x\n",
           table == ME_CORE_IMG_TABLE ? "img" : "t2img",
           table_address, firmware_entry, restart_entry);
    if (!restart_entry) { pspSdkSetK1(k1); return -8; }
    sceKernelDcacheWritebackInvalidateAll();
    int ret = wiliMeMistInit(restart_entry);
    if (ret >= 0) {
        /* Read the actual live ME table, not its SC-side firmware image. */
        Aligned64 saved = meMistGetAligned64Mem("me-original", PSP_MEMORY_PARTITION_USER, 64);
        if (saved.uid < 0) { pspSdkSetK1(k1); return -7; }
        sceKernelDcacheWritebackInvalidateRange(saved.mem, 64);
        cleanChannels();
        dmacplusFromMe(table_address + 13 * 4, (uintptr_t)saved.mem & 0x1fffffffu, DMACPLUS_BYTE_COUNT_4, 1);
        unsigned begin = sceKernelGetSystemTimeLow();
        while (hw(0xbc8001b0)) {
            if (sceKernelGetSystemTimeLow() - begin > 50000) {
                cleanChannels();
                sceKernelFreePartitionMemory(saved.uid);
                pspSdkSetK1(k1);
                return -6;
            }
            sceKernelDelayThread(10);
        }
        clearPendingOperations();
        sceKernelDcacheInvalidateRange(saved.mem, 64);
        original_entry = saved.mem[0];
        sceKernelFreePartitionMemory(saved.uid);
        MistInjector injector = {13, entry | 0x80000000u};
        meKernelMistInjectSyscall(&injector);
        ready = 1;
    }
    pspSdkSetK1(k1);
    return ret;
}

int meLabSubmit(MeJob *job)
{
    int k1 = pspSdkSetK1(0);
    if (!ready || pending) { pspSdkSetK1(k1); return -1; }
    pending = job;
    job->done = 0;
    sceKernelDcacheWritebackRange(job, sizeof(*job));
    MistTrigger trigger = {13, (void *)((uintptr_t)job | 0x80000000u)};
    int ret = meKernelMistTrigger(&trigger);
    pspSdkSetK1(k1);
    return ret;
}

int meLabWait(void)
{
    int k1 = pspSdkSetK1(0);
    unsigned begin = sceKernelGetSystemTimeLow();
    int ret = 0;
    /* The trampoline emits this flag after unwinding the complete C worker. */
    while (pending && *(volatile unsigned *)0xbfc00700 != (unsigned)-567) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) {
            sceKernelDcacheInvalidateRange(pending, sizeof(*pending));
            pending->operation = *(volatile unsigned *)0xbfc00700;
            ret = -2; break;
        }
        sceKernelDelayThread(10);
    }
    if (!ret && pending) {
        sceKernelDcacheInvalidateRange(pending, sizeof(*pending));
        if (!pending->done) ret = -3;
        pending = 0;
    }
    pspSdkSetK1(k1);
    return ret;
}
int meLabShutdown(void)
{
    int ret = meLabWait();
    if (ret < 0) return ret;
    int k1 = pspSdkSetK1(0);
    if (ready) {
        /* The completion flag is written just before returning to firmware. */
        sceKernelDelayThread(1000);
        MistInjector injector = {13, original_entry};
        meKernelMistInjectSyscall(&injector);
        ready = 0;
    }
    pspSdkSetK1(k1);
    return 0;
}
int module_start(SceSize args, void *argp) { (void)args; (void)argp; return 0; }
