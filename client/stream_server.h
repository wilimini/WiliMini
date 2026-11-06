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

/* The video bitrate sent to the PSP. The picture is always 480x272 (the server fits the source
 * into it); the names are kept for the settings file, where the value is "stream_preset". */
typedef enum {
    WILI_PRESET_PERFORMANCE = 0,
    WILI_PRESET_BALANCED = 1,
    WILI_PRESET_QUALITY = 2
} WiliStreamPreset;

/* Bilibili quality tiers of the source the server downloads. The 60 fps tiers and 4K need a
 * signed-in account; a tier the account cannot get falls back to the best one below it. */
#define WILI_QN_360P 16
#define WILI_QN_480P 32
#define WILI_QN_720P 64
#define WILI_QN_720P60 74
#define WILI_QN_1080P 80
#define WILI_QN_1080P_PLUS 112
#define WILI_QN_1080P60 116
#define WILI_QN_4K 120
#define WILI_QN_DEFAULT WILI_QN_480P
/* Frame-rate cap for the stream. The server never adds frames, so "auto" only matters for the
 * 60 fps tiers; 30 halves their rate. */
#define WILI_FPS_AUTO 60

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
    int source_qn;      /* WILI_QN_*; 0 leaves the choice to the server (480P) */
    int max_fps;        /* frame-rate cap; 0 leaves it to the server (30) */
} WiliStreamRequest;

/* 1 for one of the WILI_QN_* tiers. */
int wili_stream_quality_valid(int qn);
/* The next tier in ascending order, wrapping from the highest to the lowest. An unknown value
 * yields WILI_QN_DEFAULT. */
int wili_stream_quality_next(int qn);

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

/* Bitrates behind a preset (the server clamps them again). */
typedef struct { int video_kbps, audio_kbps; } WiliStreamParams;
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
