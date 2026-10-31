/* Custom Core bootstrap/cache helpers: mcidclan, MIT, see the upstream LICENSE.
 * This kernel adapter supplies WiliMini's bounded task mailbox and lifecycle.
 * No kcall plugin is needed: the loader already loads this kernel bridge. */
#include <pspkernel.h>
#include <pspsdk.h>
#include <psputilsforkernel.h>
#include <me-core-mapper/me-core-mapper.h>
#include "custom_core_runtime.h"
#include "me_bridge.h"
#include "me_diagnostics.h"
#include "build_diagnostics.h"
#include <errno.h>
#include <string.h>

PSP_MODULE_INFO("WiliMini Custom Core", 0x1006, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

static int ready, owned, poisoned;
static int ready_table;
static MeJob *pending;
static unsigned diagnostic_jobs;
static MeJob probe;
static MeDiagnostics diagnostics = {
    .version = ME_DIAGNOSTICS_VERSION,
    .table = -1,
    .build = WILI_DIAGNOSTIC_BUILD " " WILI_DIAGNOSTIC_COMPILED_UTC " custom-core"
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

static int wait_state(unsigned state)
{
    unsigned begin = sceKernelGetSystemTimeLow();
    while (customCoreStatus().state != state) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) return -2;
        sceKernelDelayThread(10);
    }
    return 0;
}

int meLabSubmit(MeJob *job)
{
    int k1 = pspSdkSetK1(0);
    if (!ready || pending || poisoned || customCoreStatus().command != CORE_IDLE) {
        me_diagnostic(ME_DIAG_SUBMIT, -1, ready, pending != NULL, customCoreStatus().state);
        pspSdkSetK1(k1); return -1;
    }
    pending = job;
    job->done = 0;
    sceKernelDcacheWritebackRange(job, sizeof(*job));
    customCoreSubmit(job);
    if (!diagnostic_jobs)
        me_diagnostic(ME_DIAG_SUBMIT, 0, job->operation, job->width, job->height);
    ++diagnostic_jobs;
    pspSdkSetK1(k1);
    return 0;
}

int meLabWait(void)
{
    int k1 = pspSdkSetK1(0);
    unsigned begin = sceKernelGetSystemTimeLow();
    while (pending && customCoreStatus().command != CORE_IDLE) {
        if (sceKernelGetSystemTimeLow() - begin > 500000) {
            sceKernelDcacheInvalidateRange(pending, sizeof(*pending));
            me_diagnostic(ME_DIAG_WAIT_TIMEOUT, -2, customCoreStatus().state, pending->done, pending->value);
            poisoned = 1; /* Keep ownership; another init cannot overwrite it. */
            pspSdkSetK1(k1); return -2;
        }
        sceKernelDelayThread(10);
    }
    int ret = 0;
    if (pending) {
        sceKernelDcacheInvalidateRange(pending, sizeof(*pending));
        ret = pending->done ? customCoreStatus().result : -3;
        if (diagnostic_jobs <= 1 || ret < 0)
            me_diagnostic(ME_DIAG_WAIT_DONE, ret, customCoreStatus().state, pending->done, pending->value);
        pending = NULL;
    }
    pspSdkSetK1(k1);
    return ret;
}

int meLabInit(uint32_t entry)
{
    int k1 = pspSdkSetK1(0);
    me_diagnostic(ME_DIAG_INIT, 0, ready, pending != NULL, entry);
    if (poisoned || pending) { pspSdkSetK1(k1); return -EBUSY; }
    if (ready) { pspSdkSetK1(k1); return ready_table; }
    int table = meCoreGetTableIdFromWitnessWord();
    diagnostics.table = table;
    if (table != ME_CORE_IMG_TABLE && table != ME_CORE_T2_IMG_TABLE) {
        me_diagnostic(ME_DIAG_PROFILE, -4, table, 0, 0);
        pspSdkSetK1(k1); return -4;
    }
    me_diagnostic(ME_DIAG_PROFILE, 0, table, 0, 0);
    CustomCoreBoot boot;
    int ret = customCoreStart(entry, &boot);
    if (ret < 0) { pspSdkSetK1(k1); return ret; }
    owned = 1;
    me_diagnostic(ME_DIAG_BOOTSTRAP, 0, boot.ram, boot.high, boot.low);
    me_diagnostic(ME_DIAG_CORE_BOOT, 0, boot.image_bytes, entry | 0x80000000u, boot.reset);
    ret = wait_state(CORE_READY);
    me_diagnostic(ME_DIAG_CORE_READY, ret, customCoreStatus().state, customCoreStatus().command, 0);
    if (ret < 0) {
        customCoreRestore();
        owned = ready = 0;
        poisoned = 1;
        pspSdkSetK1(k1); return ret;
    }
    ready = 1;
    ready_table = table;
    diagnostic_jobs = 0;
    /* Operation zero exercises entry/return without video buffers or LUTs. */
    memset(&probe, 0, sizeof(probe));
    ret = meLabSubmit(&probe);
    if (!ret) ret = meLabWait();
    me_diagnostic(ME_DIAG_CORE_PROBE, ret, probe.done, probe.value, customCoreStatus().state);
    if (ret < 0) { pspSdkSetK1(k1); return ret; }
    diagnostic_jobs = 0;
    me_diagnostic(ME_DIAG_READY, table, 0, 0, 0);
    pspSdkSetK1(k1);
    return table;
}

int meLabShutdown(void)
{
    me_diagnostic(ME_DIAG_SHUTDOWN, 0, ready, pending != NULL, 0);
    int ret = meLabWait();
    if (ret < 0) return ret;
    int k1 = pspSdkSetK1(0);
    if (owned) {
        customCoreStop();
        ret = wait_state(CORE_STOPPED);
        me_diagnostic(ME_DIAG_CORE_STOP, ret, customCoreStatus().state, customCoreStatus().command, 0);
        if (ret < 0) {
            poisoned = 1;
            pspSdkSetK1(k1); return ret;
        }
        me_diagnostic(ME_DIAG_RESTORE_BEGIN, 0, 0, 0, 0);
        customCoreRestore();
        owned = ready = 0;
        me_diagnostic(ME_DIAG_RESTORED, 0, 0, 0, 0);
    }
    pspSdkSetK1(k1);
    return 0;
}
int module_start(SceSize args, void *argp) { (void)args; (void)argp; return 0; }
