/* WiliMini AVC helper: the two kernel-mode steps that let the client drive the PSP's hardware
 * H.264 decoder from user mode.
 *
 *  - wiliAvcBootMode() switches the Media Engine firmware into its H.264 mode (3 = Main/CABAC,
 *    4 = Baseline/CAVLC) with sceMeBootStart660.
 *  - wiliAvcFixupImports() binds the client's sceMpeg imports to flash0:/kd/mpeg_vsh.prx, which
 *    the client loads itself. mpeg_vsh exports sceMpegGetAvcNalAu, which the game version of the
 *    library does not, and user-mode modules cannot be linked to it by the loader.
 *
 * Ported from Moonlight-PSP's moonlight_me_helper (https://github.com/k4idyn/Moonlight-PSP,
 * GPL-3.0, like WiliMini). Only firmware 6.60 and 6.61 are accepted: sceMeBootStart660 is that
 * firmware's NID. */
#include <pspsdk.h>
#include <pspkernel.h>
#include <psploadcore.h>

PSP_MODULE_INFO("WiliMini AVC Helper", 0x1006, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

int sceMeBootStart660(int mode);
SceModule *sceKernelFindModuleByUID(SceUID uid);

#define PATCHED_MAX 64
static struct { SceUID consumer; SceLibraryStubTable *stub; } patched[PATCHED_MAX];
static unsigned patched_count;

static int firmware_supported(void) {
    unsigned version = sceKernelDevkitVersion();
    return version == 0x06060010 || version == 0x06060110;
}

static int same_name(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static int was_patched(SceUID consumer, SceLibraryStubTable *stub) {
    for (unsigned i = 0; i < patched_count; ++i)
        if (patched[i].consumer == consumer && patched[i].stub == stub) return 1;
    return 0;
}

static int remember_patched(SceUID consumer, SceLibraryStubTable *stub) {
    if (was_patched(consumer, stub)) return 0;
    if (patched_count >= PATCHED_MAX) return -1;
    patched[patched_count].consumer = consumer;
    patched[patched_count].stub = stub;
    ++patched_count;
    return 0;
}

/* Returns 0 once the ME runs the requested mode; repeated calls for the same mode do nothing. */
int wiliAvcBootMode(int mode) {
    static int last_mode = -1;
    if (!firmware_supported()) return -1;
    if (mode != 3 && mode != 4) return -2;
    if (mode == last_mode) return 0;
    unsigned k1 = pspSdkSetK1(0);
    int ret = sceMeBootStart660(mode);
    pspSdkSetK1(k1);
    if (ret == 0) last_mode = mode;
    return ret;
}

static void *find_export(SceModule *module, const char *library, u32 nid) {
    if (!module || !module->ent_top || !module->ent_size) return NULL;
    SceLibraryEntryTable *entry = (SceLibraryEntryTable *)module->ent_top;
    unsigned char *end = (unsigned char *)module->ent_top + module->ent_size;
    while ((unsigned char *)entry < end) {
        if (!entry->len) break;
        if (entry->libname && same_name(entry->libname, library)) {
            unsigned count = (unsigned)entry->stubcount + entry->vstubcount;
            u32 *table = (u32 *)entry->entrytable;
            for (unsigned i = 0; table && i < count; ++i)
                if (table[i] == nid) return (void *)(unsigned long)table[i + count];
            return NULL;
        }
        entry = (SceLibraryEntryTable *)((unsigned char *)entry + (unsigned)entry->len * 4u);
    }
    return NULL;
}

/* Points every import stub of `consumer` whose NID `provider` exports at that export. Returns the
 * number of functions bound, or a negative error. The same stubs are bound again after the
 * provider was reloaded at another address. */
int wiliAvcFixupImports(SceUID consumer_uid, SceUID provider_uid) {
    if (consumer_uid < 0 || provider_uid < 0) return -1;
    unsigned k1 = pspSdkSetK1(0);
    SceModule *consumer = sceKernelFindModuleByUID(consumer_uid);
    SceModule *provider = sceKernelFindModuleByUID(provider_uid);
    int fixed = 0, failure = 0;
    if (!consumer || !provider || !consumer->stub_top || !consumer->stub_size) {
        pspSdkSetK1(k1);
        return -2;
    }
    SceLibraryStubTable *stub = (SceLibraryStubTable *)consumer->stub_top;
    unsigned char *stub_end = (unsigned char *)consumer->stub_top + consumer->stub_size;
    while ((unsigned char *)stub < stub_end) {
        if (!stub->len) { failure = -3; break; }
        unsigned entry_size = (unsigned)stub->len * 4u;
        if ((unsigned char *)stub + entry_size > stub_end) { failure = -3; break; }
        if ((stub->attribute == 9 || (stub->attribute == 1 && was_patched(consumer_uid, stub))) &&
            stub->nidtable && stub->stubtable && stub->libname) {
            unsigned count = (unsigned)stub->stubcount + stub->vstubcount;
            int fixed_here = 0;
            for (unsigned i = 0; i < count; ++i) {
                void *address = find_export(provider, stub->libname, stub->nidtable[i]);
                if (!address) continue;
                u32 *code = (u32 *)stub->stubtable + i * 2u;
                code[0] = (((u32)(unsigned long)address & 0x03FFFFFFu) >> 2) | 0x0A000000u; /* j */
                code[1] = 0;
                ++fixed_here; ++fixed;
            }
            if (fixed_here) {
                if (stub->attribute == 9) stub->attribute = 1;
                if (remember_patched(consumer_uid, stub) < 0) { failure = -4; break; }
            }
        }
        stub = (SceLibraryStubTable *)((unsigned char *)stub + entry_size);
    }
    sceKernelDcacheWritebackAll();
    sceKernelIcacheClearAll();
    pspSdkSetK1(k1);
    return failure < 0 ? failure : fixed;
}

int module_start(SceSize args, void *argp) {
    (void)args; (void)argp;
    return firmware_supported() ? 0 : -1;
}

int module_stop(SceSize args, void *argp) {
    (void)args; (void)argp;
    return 0;
}
