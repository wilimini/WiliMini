#ifndef WILI_SERVER_DISCOVERY_H
#define WILI_SERVER_DISCOVERY_H
#include "stream_server.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char host[16]; /* dotted IPv4 of the sender */
    int port;
    char name[WILI_SERVER_NAME_MAX];
} WiliFoundServer;
/* Looks for wili-server on the LAN with mDNS and the UDP beacon for up to
 * timeout_ms. Needs Wi-Fi to be connected. Returns how many servers were found
 * (at most max), or a negative value on a socket error. *cancel, when set,
 * ends the search early. */
int wili_discover_servers(WiliFoundServer *out, int max, int timeout_ms, volatile int *cancel);
#ifdef __cplusplus
}
#endif
#endif
