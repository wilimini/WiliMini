#include "http_transport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int abort_write;

static size_t discard(char *data, size_t size, size_t count, void *opaque) {
    (void)data;
    return opaque == &abort_write ? 0 : size * count;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    CURL *cached = NULL;
    for (int i = 0; i < 6; ++i) {
        CURL *curl = bili_http_transport_acquire(&cached);
        assert(curl);
        char url[256];
        snprintf(url, sizeof(url), "%s/%d", argv[1], i);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
        if (i == 4) curl_easy_setopt(curl, CURLOPT_WRITEDATA, &abort_write);
        struct curl_slist *headers = NULL;
        if (i == 0) {
            headers = curl_slist_append(headers, "Cookie: test=private");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "test=body");
        }
        CURLcode code = curl_easy_perform(curl);
        assert(code == (i == 4 ? CURLE_WRITE_ERROR : CURLE_OK));
        long status = 0, connections = -1;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connections);
        assert(status == 200);
        /* Two warm requests, followed by recovery after the server closes. */
        assert(connections == ((i == 0 || i == 3 || i == 5) ? 1 : 0));
        bili_http_transport_release(curl);
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(cached);
    curl_global_cleanup();
    puts("HTTP reuse, POST/header reset and closed-connection/aborted-write recovery passed");
    return 0;
}
