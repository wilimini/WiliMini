#ifndef WILIMINI_ME_FIRMWARE_PROFILE_H
#define WILIMINI_ME_FIRMWARE_PROFILE_H

/* MIST's live syscall tables, selected by the image witness word. */
static inline unsigned me_bridge_table_address(int table)
{
    switch (table) {
    case ME_CORE_T2_IMG_TABLE: return 0x003ce870u;
    case ME_CORE_IMG_TABLE: return 0x0018a210u;
    default: return 0;
    }
}

/* Recognize the firmware's bootstrap arguments, not a model-specific restart
 * address. Keep its RAM configuration and continuation intact. PPSSPP's ME
 * bootstrap uses the same argument sequence, with its own continuation.
 * After our first refresh the low half of a0 is a nop. */
static inline int me_bridge_bootstrap_valid(unsigned ram, unsigned high, unsigned low)
{
    return ((ram & ~3u) == 0x24090000u || (ram & ~3u) == 0x20090000u) &&
           (high & 0xffff0000u) == 0x3c040000u &&
           ((low & 0xffff0000u) == 0x34840000u || low == 0);
}
#endif
