/* Credential scope and bounded login-header parsing, shared by the PSP transport and host tests. */
#include "../bilibili.h"
#include <string.h>
#include <strings.h>

int bili_http_credential_host(const char *url) {
    const char *hosts[] = {"https://api.bilibili.com/", "https://passport.bilibili.com/",
                           "https://api.vc.bilibili.com/", "https://api.live.bilibili.com/"};
    for (unsigned i = 0; i < 4; ++i)
        if (!strncmp(url, hosts[i], strlen(hosts[i])))
            return 1;
    return 0;
}
int bili_http_login_host(const char *url) {
    const char *prefix = "https://passport.bilibili.com/";
    return !strncmp(url, prefix, strlen(prefix));
}
size_t bili_http_capture_cookie(char *data, size_t size, size_t count, void *opaque) {
    BiliHttpResponse *out = opaque;
    size_t bytes = size * count;
    if (bytes > 12 && !strncasecmp(data, "Set-Cookie:", 11)) {
        const char *start = data + 11, *end = data + bytes;
        while (start < end && (*start == ' ' || *start == '\t'))
            ++start;
        const char *stop = start;
        while (stop < end && *stop != ';' && *stop != '\r' && *stop != '\n')
            ++stop;
        size_t used = strlen(out->cookies), length = (size_t)(stop - start);
        if (length && used + length + 3 < sizeof(out->cookies)) {
            if (used) {
                memcpy(out->cookies + used, "; ", 2);
                used += 2;
            }
            memcpy(out->cookies + used, start, length);
            out->cookies[used + length] = 0;
        }
    }
    return bytes;
}
