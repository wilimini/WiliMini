#ifndef WILI_PLAYBACK_REQUEST_POLICY_H
#define WILI_PLAYBACK_REQUEST_POLICY_H
#include <string.h>
/* Only playback-address API requests get a longer deadline. */
static inline int playback_address_request(const char *url) {
    static const char host[] = "https://api.bilibili.com";
    if (!url || strncmp(url, host, sizeof(host) - 1)) return 0;
    const char *path = url + sizeof(host) - 1;
    const char *paths[] = {"/x/player/wbi/playurl", "/x/player/playurl", "/pgc/player/web/v2/playurl"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        size_t n = strlen(paths[i]);
        if (!strncmp(path, paths[i], n) && (!path[n] || path[n] == '?')) return 1;
    }
    return 0;
}
#endif
