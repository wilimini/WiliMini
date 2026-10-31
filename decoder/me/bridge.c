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
#include "me_diagnostics.h"
#include "build_diagnostics.h"
#include <stdio.h>
#include <string.h>

int wiliMeMistInit(void);

PSP_MODULE_INFO("Full ME Bridge", 0x1006, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();
static int ready;
static MeJob *pending;
static unsigned original_entry;
static int ready_table;
static unsigned diagnostic_jobs;
static MeDiagnostics diagnostics = {
    .version = ME_DIAGNOSTICS_VERSION,
    .table = -1,
    .build = WILI_DIAGNOSTIC_BUILD " " WILI_DIAGNOSTIC_COMPILED_UTC
};
static void me_diagnostic(unsigned event, int result, unsigned a, unsigned b, unsigned c)
{
    int intr = sceKernelCpuSuspendIntr();
    unsigned sequence = ++diagnostics.sequence;
    diagnostics.records[(sequence - 1) % ME_DIAGNOSTICS_CAPACITY] =
        (MeDiagnosticRecord){sequence, sceKernelGetSystemTimeLow(), event, result, a, b, c};
    sceKernelCpuResumeIntrWithSync(intr);
}
int meLabGetDiagnostics(MeDiagnostics *out, unsigned size)
{
    if (!out || size != sizeof(*out)) return -1;
    int k1 = pspSdkSetK1(0);
    int intr = sceKernelCpuSuspendIntr();
    diagnostics.ready = ready;
    diagnostics.pending = pending != NULL;
    memcpy(out, &diagnostics, sizeof(*out));
    sceKernelCpuResumeIntrWithSync(intr);
    pspSdkSetK1(k1);
    return 0;
}

int meLabBackend(void) { return ME_BACKEND_MIST; }

/* MIST user-mode entry points are unused in this kernel-only bridge. */
int meMistCall(FPCall f, void *p) { return f(p); }
int kCall_2(FCall f, unsigned int seg) { return ((FCall)((uintptr_t)f | seg))(); }
int kCall_3(FPCall f, unsigned int seg, void *p) { return ((FPCall)((uintptr_t)f | seg))(p); }

int meLabInit(uint32_t entry)
{
    int k1 = pspSdkSetK1(0);
    me_diagnostic(ME_DIAG_INIT, 0, ready, pending != NULL, entry);
    if (ready) { pspSdkSetK1(k1); return ready_table; }
    int table = meCoreGetTableIdFromWitnessWord();
    diagnostics.table = table;
    unsigned table_address = me_bridge_table_address(table);
    me_diagnostic(ME_DIAG_PROFILE, table_address ? 0 : -100 - table, table, table_address, 0);
    if (!table_address) { pspSdkSetK1(k1); return -100 - table; }
    unsigned ram = hw(0xbfc00060), high = hw(0xbfc00064), low = hw(0xbfc00068);
    me_diagnostic(ME_DIAG_BOOTSTRAP, me_bridge_bootstrap_valid(ram, high, low) ? 0 : -8, ram, high, low);
    printf("ME profile=%s table=0x%08x bootstrap=%08x/%08x/%08x\n",
           table == ME_CORE_IMG_TABLE ? "img" : "t2img",
           table_address, ram, high, low);
    if (!me_bridge_bootstrap_valid(ram, high, low)) { pspSdkSetK1(k1); return -8; }
    sceKernelDcacheWritebackInvalidateAll();
    int ret = wiliMeMistInit();
    me_diagnostic(ME_DIAG_BACKEND, ret, 0, 0, 0);
    if (ret >= 0) {
        /* Read the actual live ME table, not its SC-side firmware image. */
        Aligned64 saved = meMistGetAligned64Mem("me-original", PSP_MEMORY_PARTITION_USER, 64);
        if (saved.uid < 0) {
            me_diagnostic(ME_DIAG_DMA_BEGIN, -7, table_address + 13 * 4, saved.uid, 0);
            pspSdkSetK1(k1); return -7;
        }
        sceKernelDcacheWritebackInvalidateRange(saved.mem, 64);
        cleanChannels();
        me_diagnostic(ME_DIAG_DMA_BEGIN, 0, table_address + 13 * 4,
                      (uintptr_t)saved.mem & 0x1fffffffu, hw(0xbc8001b0));
        dmacplusFromMe(table_address + 13 * 4, (uintptr_t)saved.mem & 0x1fffffffu, DMACPLUS_BYTE_COUNT_4, 1);
        unsigned begin = sceKernelGetSystemTimeLow();
        while (hw(0xbc8001b0)) {
            if (sceKernelGetSystemTimeLow() - begin > 50000) {
                me_diagnostic(ME_DIAG_DMA_END, -6, hw(0xbc8001b0),
                              sceKernelGetSystemTimeLow() - begin, 0);
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
        me_diagnostic(ME_DIAG_DMA_END, 0, hw(0xbc8001b0),
                      sceKernelGetSystemTimeLow() - begin, original_entry);
        sceKernelFreePartitionMemory(saved.uid);
        MistInjector injector = {13, entry | 0x80000000u};
        me_diagnostic(ME_DIAG_INJECT_BEGIN, 0, injector.index, injector.addr, original_entry);
        meKernelMistInjectSyscall(&injector);
        ready = 1;
        ready_table = table;
        diagnostic_jobs = 0;
        me_diagnostic(ME_DIAG_READY, table, 0, 0, 0);
        ret = table;
    }
    pspSdkSetK1(k1);
    return ret;
}

int meLabSubmit(MeJob *job)
{
    int k1 = pspSdkSetK1(0);
    if (!ready || pending) {
        me_diagnostic(ME_DIAG_SUBMIT, -1, ready, pending != NULL, 0);
        pspSdkSetK1(k1); return -1;
    }
    pending = job;
    job->done = 0;
    sceKernelDcacheWritebackRange(job, sizeof(*job));
    MistTrigger trigger = {13, (void *)((uintptr_t)job | 0x80000000u)};
    if (!diagnostic_jobs)
        me_diagnostic(ME_DIAG_SUBMIT, 0, job->operation, job->width, job->height);
    int ret = meKernelMistTrigger(&trigger);
    if (!diagnostic_jobs || ret < 0)
        me_diagnostic(ME_DIAG_TRIGGER, ret, (uintptr_t)trigger.param, job->done, job->value);
    ++diagnostic_jobs;
    pspSdkSetK1(k1);
    return ret;
}

int meLabWait(void)
{
    int k1 = pspSdkSetK1(0);
    unsigned begin = sceKernelGetSystemTimeLow();
    int ret = 0;
    /* The trampoline emits this flag after unwinding the complete C worker. */
    while (pending && hw(0xbfc00700) != (unsigned)-567) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) {
            sceKernelDcacheInvalidateRange(pending, sizeof(*pending));
            me_diagnostic(ME_DIAG_WAIT_TIMEOUT, -2, hw(0xbfc00700), pending->done, pending->value);
            pending->operation = hw(0xbfc00700);
            ret = -2; break;
        }
        sceKernelDelayThread(10);
    }
    if (!ret && pending) {
        sceKernelDcacheInvalidateRange(pending, sizeof(*pending));
        if (!pending->done) ret = -3;
        if (diagnostic_jobs == 1 || ret < 0)
            me_diagnostic(ME_DIAG_WAIT_DONE, ret, hw(0xbfc00700), pending->done, pending->value);
        pending = 0;
    }
    pspSdkSetK1(k1);
    return ret;
}
int meLabShutdown(void)
{
    me_diagnostic(ME_DIAG_SHUTDOWN, 0, ready, pending != NULL, 0);
    int ret = meLabWait();
    if (ret < 0) return ret;
    int k1 = pspSdkSetK1(0);
    if (ready) {
        /* The completion flag is written just before returning to firmware. */
        sceKernelDelayThread(1000);
        MistInjector injector = {13, original_entry};
        me_diagnostic(ME_DIAG_RESTORE_BEGIN, 0, injector.index, injector.addr, 0);
        meKernelMistInjectSyscall(&injector);
        ready = 0;
        me_diagnostic(ME_DIAG_RESTORED, 0, 0, 0, 0);
    }
    pspSdkSetK1(k1);
    return 0;
}
int module_start(SceSize args, void *argp) { (void)args; (void)argp; return 0; }
