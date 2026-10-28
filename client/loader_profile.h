#ifndef WILIMINI_LOADER_PROFILE_H
#define WILIMINI_LOADER_PROFILE_H

/* Model 0 is PSP-1000. Failed model queries use the smaller client.
 * Leave room for code/BSS, threads and network pools beyond the 24 MiB heap. */
static inline int loader_use_extended_client(int model, unsigned max_free) {
    return model > 0 && max_free >= 36u * 1024 * 1024;
}
#endif
