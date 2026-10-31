/* Bootstrap and cache helpers by mcidclan, MIT; see the upstream LICENSE.
 * Keep this hardware layer separate from the bridge's ownership policy. */
#include <pspkernel.h>
#include <psputilsforkernel.h>
#include <psppower.h>
#include <me-core-mapper/me-core.h>
#include "custom_core_runtime.h"
#include <string.h>

/* Exactly one cache line. SC and ME use uncached aliases for every access. */
static struct __attribute__((aligned(64))) {
    volatile unsigned command, state, entry, job;
    volatile int result;
} control;
#define core ((volatile __typeof__(control) *)customCoreUncachedAddress((uintptr_t)&control))
#define core_sync() __asm__ volatile("sync" ::: "memory")

static unsigned saved_boot[128];
static unsigned saved_reset;

/* Entered by upstream meLibHandler with its FAT/T2 stack and memory setup.
 * Publish completion only after the client's entire C worker has returned. */
void meLibOnProcess(void)
{
    meLibDcacheWritebackInvalidateAll();
    meLibIcacheInvalidateAll();
    core->state = CORE_READY;
    core_sync();
    for (;;) {
        unsigned command = core->command;
        core_sync();
        if (command == CORE_JOB) {
            core->state = CORE_RUNNING;
            int (*worker)(int, void *) = (void *)(uintptr_t)core->entry;
            void *job = (void *)(uintptr_t)core->job;
            int result = worker(0, job);
            core_sync();
            core->result = result;
            core->state = CORE_READY;
            core_sync();
            core->command = CORE_IDLE;
            core_sync();
        } else if (command == CORE_STOP) {
            core->state = CORE_STOPPED;
            core_sync();
            /* The SC asserts reset before releasing any code or buffers.
             * Remain in this bridge even if an interrupt wakes HALT. */
            for (;;) __asm__ volatile(".word 0x70000000; nop" ::: "memory");
        } else {
            __asm__ volatile("nop; nop; nop; nop");
        }
    }
}

CustomCoreStatus customCoreStatus(void)
{
    CustomCoreStatus status = {core->command, core->state, core->result};
    core_sync();
    return status;
}
void customCoreSubmit(MeJob *job)
{
    core->job = (uintptr_t)job | 0x80000000u;
    core->result = 0;
    core_sync();
    core->command = CORE_JOB;
    core_sync();
}
void customCoreStop(void)
{
    core->command = CORE_STOP;
    core_sync();
}
int customCoreStart(uint32_t entry, CustomCoreBoot *boot)
{
    unsigned size = &__stop__me_section - &__start__me_section;
    if (!entry || size > sizeof(saved_boot)) return -4;
    /* Firmware suspend RPC cannot service a core running our task loop. Defer
     * the power switch until firmware is restored, rather than suspend mid-job.
     * Full context save/restore is deliberately outside this opt-in backend. */
    int ret = scePowerLock(0);
    if (ret < 0) return ret;
    saved_reset = HW_SYS_RESET_ENABLE;
    for (unsigned i = 0; i < sizeof(saved_boot) / sizeof(saved_boot[0]); ++i)
        saved_boot[i] = hw(ME_HANDLER_BASE + i * 4);
    *boot = (CustomCoreBoot){size, saved_reset, saved_boot[0x60 / 4],
                            saved_boot[0x64 / 4], saved_boot[0x68 / 4]};
    memset(&control, 0, sizeof(control));
    sceKernelDcacheWritebackInvalidateRange(&control, sizeof(control));
    core->entry = entry | 0x80000000u;
    core_sync();
    /* Upstream Custom Core bootstrap, called directly by the kernel bridge.
     * Stop the old core before replacing its SRAM instructions. */
    int intr = sceKernelCpuSuspendIntr();
    HW_SYS_RESET_ENABLE = saved_reset | 0x14;
    core_sync();
    memcpy((void *)ME_HANDLER_BASE, &__start__me_section, size);
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    HW_SYS_RESET_ENABLE = saved_reset;
    core_sync();
    sceKernelCpuResumeIntrWithSync(intr);
    return 0;
}
/* The bridge calls this after a stop acknowledgement, or a boot failure before
 * any client task. Reset stops all ME instructions before restoring firmware. */
void customCoreRestore(void)
{
    int intr = sceKernelCpuSuspendIntr();
    HW_SYS_RESET_ENABLE = saved_reset | 0x14;
    core_sync();
    for (unsigned i = 0; i < sizeof(saved_boot) / sizeof(saved_boot[0]); ++i)
        hw(ME_HANDLER_BASE + i * 4) = saved_boot[i];
    core_sync();
    HW_SYS_RESET_ENABLE = saved_reset;
    core_sync();
    sceKernelCpuResumeIntrWithSync(intr);
    scePowerUnlock(0);
}
