#ifndef WILIMINI_ME_BACKEND_SELECT_H
#define WILIMINI_ME_BACKEND_SELECT_H

#include <stddef.h>
#include <string.h>

/* Numeric values match ME_BACKEND_* in decoder/me/me_bridge.h and
 * pspapp::MeBackend in client/preferences.hpp. */
#define WILI_ME_BACKEND_MIST 0
#define WILI_ME_BACKEND_CUSTOM_CORE 1

#define WILI_ME_BRIDGE_MIST "me_bridge.prx"
#define WILI_ME_BRIDGE_CUSTOM_CORE "me_bridge_custom_core.prx"

static inline const char *wili_me_bridge_file(int backend) {
    return backend == WILI_ME_BACKEND_CUSTOM_CORE ? WILI_ME_BRIDGE_CUSTOM_CORE
                                                  : WILI_ME_BRIDGE_MIST;
}

static inline const char *wili_me_backend_name(int backend) {
    return backend == WILI_ME_BACKEND_CUSTOM_CORE ? "custom-core" : "mist";
}

/* The loader runs before the C++ client, so it only looks for the
 * "me_backend" string in settings.json written by preferences.cpp. Only an
 * explicit "mist" selects MIST; anything missing, malformed or unknown selects
 * WILI_ME_BACKEND_CUSTOM_CORE. The client validates the same file strictly and
 * reports problems itself. */
static inline int wili_me_backend_from_settings(const char *text, size_t length) {
    static const char key[] = "\"me_backend\"";
    static const char custom[] = "custom-core";
    static const char mist[] = "mist";
    if (!text) return WILI_ME_BACKEND_CUSTOM_CORE;
    const size_t key_length = sizeof(key) - 1;
    for (size_t i = 0; i + key_length <= length; ++i) {
        if (memcmp(text + i, key, key_length) != 0) continue;
        size_t p = i + key_length;
        while (p < length && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) ++p;
        if (p >= length || text[p] != ':') return WILI_ME_BACKEND_CUSTOM_CORE;
        ++p;
        while (p < length && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) ++p;
        if (p >= length || text[p] != '"') return WILI_ME_BACKEND_CUSTOM_CORE;
        ++p;
        if (p + sizeof(custom) - 1 < length && memcmp(text + p, custom, sizeof(custom) - 1) == 0 &&
            text[p + sizeof(custom) - 1] == '"')
            return WILI_ME_BACKEND_CUSTOM_CORE;
        if (p + sizeof(mist) - 1 < length && memcmp(text + p, mist, sizeof(mist) - 1) == 0 &&
            text[p + sizeof(mist) - 1] == '"')
            return WILI_ME_BACKEND_MIST;
        return WILI_ME_BACKEND_CUSTOM_CORE;
    }
    return WILI_ME_BACKEND_CUSTOM_CORE;
}

#endif
