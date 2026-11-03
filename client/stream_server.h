#ifndef WILI_STREAM_SERVER_H
#define WILI_STREAM_SERVER_H
/* Client side of the wili-server protocol (see docs/streaming-server.md).
 * Pure C with no PSP headers so the host tests can run it. Everything here is
 * bounds-checked: callers pass capacities and get -1 on overflow. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define WILI_PROTO 1
#define WILI_SERVER_PORT 8787
#define WILI_BEACON_PORT 47612
#define WILI_SERVER_HOST_MAX 64
#define WILI_SERVER_TOKEN_MAX 33
#define WILI_SERVER_NAME_MAX 49

typedef struct {
    int enabled;
    char host[WILI_SERVER_HOST_MAX];
    int port;
    char token[WILI_SERVER_TOKEN_MAX];
} WiliServer;

typedef enum {
    WILI_PRESET_PERFORMANCE = 0,
    WILI_PRESET_BALANCED = 1,
    WILI_PRESET_QUALITY = 2
} WiliStreamPreset;

typedef enum {
    WILI_PROFILE_AUTO = 0,
    WILI_PROFILE_CAVLC = 1,
    WILI_PROFILE_CABAC = 2
} WiliStreamProfile;

typedef struct {
    const char *bvid;   /* ignored when pgc */
    long long cid;
    long long start_ms; /* where playback resumes; the server seeks there */
    int pgc;            /* bangumi / film episode */
    WiliStreamPreset preset;
    WiliStreamProfile profile;
} WiliStreamRequest;

/* "192.168.1.20", "192.168.1.20:8787", "desk.local:9000". Writes the host and
 * the port (WILI_SERVER_PORT when omitted). Returns 0, or -1 for text that is
 * not a plausible host name or address. */
int wili_server_parse_address(const char *text, char *host, size_t host_cap, int *port);
/* 4 to 32 characters of [A-Za-z0-9_-]. */
int wili_server_valid_token(const char *token);
/* Enabled, with a host, a port and a token. */
int wili_server_ready(const WiliServer *server);

/* Hosts whose API calls are routed through the server.
 * Returns 1 when url was rewritten into out, 0 when it is not an https URL for
 * such a host (out is untouched), -1 when out is too small. */
int wili_server_rewrite(const WiliServer *server, const char *url, char *out, size_t cap);
int wili_server_hello_url(const WiliServer *server, char *out, size_t cap);
int wili_server_logout_url(const WiliServer *server, char *out, size_t cap);
int wili_server_stream_url(const WiliServer *server, const WiliStreamRequest *request,
                           char *out, size_t cap);

/* Video parameters behind a preset (the server clamps them again). */
typedef struct { int width, height, fps, video_kbps, audio_kbps; } WiliStreamParams;
WiliStreamParams wili_stream_params(WiliStreamPreset preset);

/* Discovery wire formats. Each parser returns 0 and fills port/name, or -1. */
int wili_beacon_probe(unsigned char *out, size_t cap);
int wili_beacon_parse(const unsigned char *packet, size_t length, int *port,
                      char *name, size_t name_cap);
int wili_mdns_query(unsigned char *out, size_t cap);
int wili_mdns_parse(const unsigned char *packet, size_t length, int *port,
                    char *name, size_t name_cap);

#ifdef __cplusplus
}
#endif
#endif
