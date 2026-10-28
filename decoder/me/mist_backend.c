/* Project adapter for mcidclan's MIT-licensed MIST implementation.
 * Keep the upstream source and its license intact. The only changed operation
 * is ME refresh: the bridge supplies the image-specific restart entry.
 * Refresh sequence adapted from me-mist.c, Copyright (c) 2026 mcidclan.
 * See LICENSE.MIST.txt and the upstream LICENSE.md for the MIT terms. */
#define meKernelMistInit wili_me_upstream_init
#include <me-mist.c>
#undef meKernelMistInit

int wiliMeMistInit(unsigned restart_entry)
{
    int ret = init(meKernelMistSelectTable());
    if (ret < 0) return ret;
    int intr = sceKernelCpuSuspendIntr();
    hw(0xbfc0071c) = restart_entry;
    hw(0xbfc00704) = 0x1f;
    meMistSync();
    hw(0xbfc00060) = 0x20090003;
    hw(0xbfc00064) = 0x3c048840;
    hw(0xbfc00068) = 0;
    meMistSync();
    hw(0xbc10004c) |= 0x34;
    hw(0xbc10004c) = 0;
    meMistSync();
    sceKernelCpuResumeIntrWithSync(intr);
    return 0;
}
