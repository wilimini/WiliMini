#ifndef WILIMINI_ME_FIRMWARE_PROFILE_H
#define WILIMINI_ME_FIRMWARE_PROFILE_H

/* Values correspond to mcidclan's MIST init() and mistRefreshMe().
 * Fat refresh offsets are upstream candidates, not a verified firmware list.
 * Accept one only when firmware has already installed that exact entry in
 * the ME restart register; never guess between the two candidates. */
static inline unsigned me_bridge_table_address(int table)
{
    switch (table) {
    case ME_CORE_T2_IMG_TABLE: return 0x003ce870u;
    case ME_CORE_IMG_TABLE: return 0x0018a210u;
    default: return 0;
    }
}

static inline unsigned me_bridge_restart_entry(int table, unsigned firmware_entry)
{
    if (table == ME_CORE_T2_IMG_TABLE) return 0x00071e6cu;
    if (table == ME_CORE_IMG_TABLE &&
        (firmware_entry == 0x000828e0u || firmware_entry == 0x000824dcu))
        return firmware_entry;
    return 0;
}
#endif
