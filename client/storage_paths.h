#ifndef WILIMINI_STORAGE_PATHS_H
#define WILIMINI_STORAGE_PATHS_H

#include <stdio.h>
#include <string.h>

#define WILI_STORAGE_MS0 "ms0:/PSP/GAME/WiliMini"
#define WILI_STORAGE_EF0 "ef0:/PSP/GAME/WiliMini"
#define WILI_STORAGE_HOST0 "host0:/dist/PSP/GAME/WiliMini"

static const char *const wili_storage_roots[] = {
    WILI_STORAGE_MS0, WILI_STORAGE_EF0, WILI_STORAGE_HOST0
};

/* Select a complete package before loading any module, so versions cannot mix. */
static inline const char *wili_select_storage(int (*file_exists)(const char *), int require_me) {
    static const char *const files[] = {
        "me_bridge.prx", "me_bridge_custom_core.prx",
        "wili_mini.prx", "wili_mini_1000.prx", "cacert.pem", "wqy.bmf"
    };
    for (unsigned i = 0; i < sizeof(wili_storage_roots) / sizeof(wili_storage_roots[0]); ++i) {
        unsigned j;
        for (j = 0; j < sizeof(files) / sizeof(files[0]); ++j) {
            if (j < 2 && !require_me) continue;
            char path[128];
            snprintf(path, sizeof(path), "%s/%s", wili_storage_roots[i], files[j]);
            if (!file_exists(path)) break;
        }
        if (j == sizeof(files) / sizeof(files[0])) return wili_storage_roots[i];
    }
    return NULL;
}

/* The loader already passes the absolute client filename as its module argument. */
static inline const char *wili_storage_from_client(const char *path) {
    if (!path) return NULL;
    for (unsigned i = 0; i < sizeof(wili_storage_roots) / sizeof(wili_storage_roots[0]); ++i) {
        const size_t length = strlen(wili_storage_roots[i]);
        if (strncmp(path, wili_storage_roots[i], length) == 0 &&
            (strcmp(path + length, "/wili_mini.prx") == 0 ||
             strcmp(path + length, "/wili_mini_1000.prx") == 0))
            return wili_storage_roots[i];
    }
    return NULL;
}

#endif
