/* Exercise the production bridge's init/restore with hardware calls mocked. */
#include <assert.h>
#include <stdio.h>
#include "bridge_mock.h"
static unsigned restart_register, dma_status, ticks, scratch[16];
static unsigned injected_address, dma_source, selected_restart;
static int table_id, current_k1 = 17, init_calls, inject_calls, alloc_fail;
static int backend_error, timeout_dma, allocations;
unsigned *test_register(unsigned address) {
    if (address == 0xbfc0071c) return &restart_register;
    assert(address == 0xbc8001b0);
    return &dma_status;
}
int pspSdkSetK1(int value) { int old = current_k1; current_k1 = value; return old; }
int meCoreGetTableIdFromWitnessWord(void) { return table_id; }
int wiliMeMistInit(unsigned entry) { ++init_calls; selected_restart = entry; return backend_error; }
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
int meKernelMistTrigger(MistTrigger *p) { (void)p; assert(0); return -1; }
#include "../decoder/me/bridge.c"

int main(void) {
    const unsigned entries[] = {0x71e6c, 0x828e0, 0x824dc};
    for (unsigned i = 0; i < 3; ++i) {
        table_id = i ? ME_CORE_IMG_TABLE : ME_CORE_T2_IMG_TABLE;
        restart_register = i ? entries[i] : 0;
        unsigned expected_table = i ? 0x18a210 : 0x3ce870;
        assert(meLabInit(0x08801000) == 0);
        assert(current_k1 == 17 && selected_restart == entries[i]);
        assert(dma_source == expected_table + 13 * 4);
        assert(injected_address == 0x88801000 && allocations == 0);
        int before = init_calls;
        assert(meLabInit(0x08801000) == 0 && init_calls == before);
        assert(meLabShutdown() == 0 && injected_address == 0x80012340);
        assert(current_k1 == 17 && !ready);
    }
    int before = init_calls;
    table_id = ME_CORE_IMG_TABLE;
    restart_register = 0x71e6c; /* Never use Slim restart on Fat. */
    assert(meLabInit(0x08801000) == -8 && init_calls == before);
    table_id = -1;
    assert(meLabInit(0x08801000) == -99 && init_calls == before);
    table_id = ME_CORE_T2_IMG_TABLE;
    backend_error = -4;
    assert(meLabInit(0x08801000) == -4 && !ready);
    backend_error = 0; alloc_fail = 1;
    before = inject_calls;
    assert(meLabInit(0x08801000) == -7 && inject_calls == before && !ready);
    alloc_fail = 0; timeout_dma = 1;
    assert(meLabInit(0x08801000) == -6 && allocations == 0);
    assert(inject_calls == before && !ready && current_k1 == 17);
    puts("ME IMG/T2 init, live-table restore, unsupported restart and DMA failure cleanup: passed");
    return 0;
}
