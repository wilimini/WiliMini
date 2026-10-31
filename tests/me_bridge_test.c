/* Exercise the production bridge's init/restore with hardware calls mocked. */
#include <assert.h>
#include <stdio.h>
#include "bridge_mock.h"
static unsigned bootstrap_ram = 0x20090003, bootstrap_high = 0x3c048840;
static unsigned bootstrap_low = 0x34840000, mailbox, dma_status, ticks, scratch[16];
static unsigned injected_address, dma_source;
static int table_id, current_k1 = 17, init_calls, inject_calls, alloc_fail;
static int backend_error, timeout_dma, allocations;
static int interrupts_disabled;
int sceKernelCpuSuspendIntr(void) {
    int old = interrupts_disabled; interrupts_disabled = 1; return old;
}
void sceKernelCpuResumeIntrWithSync(int state) { interrupts_disabled = state; }
unsigned *test_register(unsigned address) {
    if (address == 0xbfc00060) return &bootstrap_ram;
    if (address == 0xbfc00064) return &bootstrap_high;
    if (address == 0xbfc00068) return &bootstrap_low;
    if (address == 0xbfc00700) return &mailbox;
    assert(address == 0xbc8001b0);
    return &dma_status;
}
int pspSdkSetK1(int value) { int old = current_k1; current_k1 = value; return old; }
int meCoreGetTableIdFromWitnessWord(void) { return table_id; }
int wiliMeMistInit(void) { ++init_calls; return backend_error; }
void sceKernelDcacheWritebackInvalidateAll(void) {}
void sceKernelDcacheWritebackInvalidateRange(void *p, unsigned n) { (void)p; (void)n; }
void sceKernelDcacheInvalidateRange(void *p, unsigned n) { (void)p; (void)n; }
void sceKernelDcacheWritebackRange(void *p, unsigned n) { (void)p; (void)n; }
Aligned64 meMistGetAligned64Mem(char *name, int partition, unsigned n) {
    (void)name; assert(partition == 2 && n == 64);
    if (alloc_fail) return (Aligned64){0, -1};
    ++allocations;
    return (Aligned64){scratch, 1};
}
void cleanChannels(void) { dma_status = 0; }
void clearPendingOperations(void) {}
void dmacplusFromMe(unsigned src, unsigned dst, unsigned width, unsigned count) {
    (void)dst; assert(width == 2 && count == 1);
    dma_source = src; scratch[0] = 0x80012340;
    dma_status = timeout_dma;
}
unsigned sceKernelGetSystemTimeLow(void) { ticks += 10000; return ticks; }
void sceKernelDelayThread(unsigned n) { (void)n; }
void sceKernelFreePartitionMemory(int uid) { assert(uid == 1); --allocations; }
void meKernelMistInjectSyscall(MistInjector *p) {
    assert(p->index == 13); ++inject_calls; injected_address = p->addr;
}
int meKernelMistTrigger(MistTrigger *p) { assert(p->index == 13); mailbox = 0; return 0; }
#include "../decoder/me/bridge.c"

static MeDiagnosticRecord last_event(unsigned event) {
    MeDiagnostics snapshot;
    assert(meLabGetDiagnostics(&snapshot, sizeof(snapshot)) == 0);
    assert(current_k1 == 17 && snapshot.version == ME_DIAGNOSTICS_VERSION);
    unsigned first = snapshot.sequence > ME_DIAGNOSTICS_CAPACITY ?
        snapshot.sequence - ME_DIAGNOSTICS_CAPACITY : 0;
    for (unsigned i = snapshot.sequence; i > first; --i) {
        MeDiagnosticRecord record = snapshot.records[(i - 1) % ME_DIAGNOSTICS_CAPACITY];
        if (record.event == event) return record;
    }
    assert(!"missing diagnostic event");
    return (MeDiagnosticRecord){0};
}
int main(void) {
    assert(meLabBackend() == ME_BACKEND_MIST);
    for (unsigned i = 0; i < 2; ++i) {
        table_id = i ? ME_CORE_IMG_TABLE : ME_CORE_T2_IMG_TABLE;
        unsigned expected_table = i ? 0x18a210 : 0x3ce870;
        assert(meLabInit(0x08801000) == table_id);
        assert(current_k1 == 17 && dma_source == expected_table + 13 * 4);
        assert(injected_address == 0x88801000 && allocations == 0);
        assert(last_event(ME_DIAG_PROFILE).b == expected_table);
        int before = init_calls;
        assert(meLabInit(0x08801000) == table_id && init_calls == before);
        assert(meLabShutdown() == 0 && injected_address == 0x80012340);
        assert(current_k1 == 17 && !ready);
        assert(last_event(ME_DIAG_RESTORED).result == 0);
    }
    int before = init_calls;
    bootstrap_high = 0;
    assert(meLabInit(0x08801000) == -8 && init_calls == before);
    assert(last_event(ME_DIAG_BOOTSTRAP).result == -8);
    bootstrap_high = 0x3c048840;
    table_id = -1;
    assert(meLabInit(0x08801000) == -99 && init_calls == before);
    table_id = ME_CORE_T2_IMG_TABLE;
    backend_error = -4;
    assert(meLabInit(0x08801000) == -4 && !ready);
    assert(last_event(ME_DIAG_BACKEND).result == -4);
    backend_error = 0; alloc_fail = 1;
    before = inject_calls;
    assert(meLabInit(0x08801000) == -7 && inject_calls == before && !ready);
    alloc_fail = 0; timeout_dma = 0x1234;
    assert(meLabInit(0x08801000) == -6 && allocations == 0);
    assert(inject_calls == before && !ready && current_k1 == 17);
    MeDiagnosticRecord timeout = last_event(ME_DIAG_DMA_END);
    assert(timeout.result == -6 && timeout.a == 0x1234 && timeout.b > 50000);
    assert(dma_status == 0); /* Log preserves the status from before cleanup. */

    timeout_dma = 0;
    assert(meLabInit(0x08801000) == table_id);
    MeJob job = {.operation = 1, .width = 640, .height = 360};
    assert(meLabSubmit(&job) == 0);
    job.value = 5;
    assert(meLabWait() == -2);
    timeout = last_event(ME_DIAG_WAIT_TIMEOUT);
    assert(timeout.result == -2 && timeout.b == 0 && timeout.c == 5);
    assert(meLabShutdown() == -2 && pending == &job && ready);
    assert(meLabSubmit(&job) == -1); /* Still owns the timed-out buffer. */
    job.done = 1; mailbox = (unsigned)-567;
    assert(meLabWait() == 0 && pending == NULL);
    assert(last_event(ME_DIAG_WAIT_DONE).b == 1);
    assert(meLabShutdown() == 0 && !ready);

    MeDiagnostics snapshot;
    assert(meLabGetDiagnostics(NULL, sizeof(snapshot)) == -1);
    assert(meLabGetDiagnostics(&snapshot, sizeof(snapshot) - 1) == -1);
    for (int i = 0; i < 40; ++i) assert(meLabSubmit(&job) == -1);
    assert(meLabGetDiagnostics(&snapshot, sizeof(snapshot)) == 0);
    for (unsigned i = snapshot.sequence - ME_DIAGNOSTICS_CAPACITY; i < snapshot.sequence; ++i)
        assert(snapshot.records[i % ME_DIAGNOSTICS_CAPACITY].sequence == i + 1);
    assert(snapshot.ready == 0 && snapshot.pending == 0 && current_k1 == 17 && !interrupts_disabled);
    puts("ME init/restore, DMA timeout snapshot, task timeout ownership and bounded diagnostic ring: passed");
    return 0;
}
