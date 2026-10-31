#ifndef BILI_HTTP_TRANSPORT_H
#define BILI_HTTP_TRANSPORT_H

#include <curl/curl.h>

/* Owned exclusively by the serialized API worker. Reset options, not the
 * connection/DNS/TLS caches. Do not enable curl's persistent cookie engine. */
static CURL *bili_http_transport_acquire(CURL **cached) {
    if (!*cached) *cached = curl_easy_init();
    if (!*cached) return NULL;
    curl_easy_reset(*cached);
    /* API and passport are the common origins; bound idle sockets on PSP. */
    curl_easy_setopt(*cached, CURLOPT_MAXCONNECTS, 2L);
    return *cached;
}

static void bili_http_transport_release(CURL *cached) {
    /* Drop stack callbacks, POST data and credential headers before returning. */
    curl_easy_reset(cached);
}

#endif
