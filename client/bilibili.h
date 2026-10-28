#ifndef BILIBILI_H
#define BILIBILI_H
#include <stddef.h>
#include "storage_paths.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "psp_video.h"
#define HOME_COUNT 6
#define BILI_STREAM_SEGMENTS 8
#define BILI_MEDIA_URLS 4
/* Selected once at startup. Literal paths remain absolute and have static lifetime. */
extern int client_storage_index;
#define CLIENT_PATH(suffix) (client_storage_index == 0 ? WILI_STORAGE_MS0 suffix : \
                            client_storage_index == 1 ? WILI_STORAGE_EF0 suffix : \
                                                        WILI_STORAGE_HOST0 suffix)
#define CLIENT_ROOT CLIENT_PATH("")
extern const char *client_storage_device;
typedef struct {
    char bvid[24], title[256], author[96], picture[512];
    long long cid, views;
    int duration;
} BiliVideo;
typedef struct { BiliVideo videos[HOME_COUNT]; int count, page, cached; } BiliHome;
typedef struct {
    char url[4096];
    unsigned long long size;
    int count;
    struct {
        char url[4096];
        unsigned long long size;
    } segments[BILI_STREAM_SEGMENTS];
} BiliStream;
typedef struct BiliStreamCache BiliStreamCache;
typedef struct BiliRangeStream BiliRangeStream;
typedef struct BiliLiveStream BiliLiveStream;
typedef struct {
    const char *url;
    const char *backups[BILI_MEDIA_URLS - 1];
    int backup_count;
} BiliMediaUrl;
typedef struct {
    int streaming;
    char path[256];
    BiliStreamCache *stream;
    BiliRangeStream *video;
    BiliRangeStream *audio;
    BiliLiveStream *live;
} BiliPlayback;
int bili_parse_home(const char *text, BiliHome *out);
int bili_parse_stream(const char *text, BiliStream *out);
int bili_home(int page, BiliHome *out);
int bili_prepare_playback(const BiliVideo *video, BiliPlayback *out);
int bili_prepare_stream_playback(const BiliStream *stream, const char *cache_key, BiliPlayback *out);
int bili_prepare_live_playback(const char *url, BiliPlayback *out);
const PspVideoSource *bili_live_source(BiliLiveStream *stream, int audio);
int bili_live_finish(BiliLiveStream *stream);
int bili_prepare_range_playback(const BiliVideo *video, const BiliMediaUrl *video_url,
                                const BiliMediaUrl *audio_url, BiliPlayback *out);
const PspVideoSource *bili_playback_source(const BiliPlayback *playback);
const PspVideoSource *bili_playback_audio_source(const BiliPlayback *playback);
int bili_finish_playback(BiliPlayback *playback);
int network_init(void);
int network_connect(void);
int network_ready(void);
const char *network_progress_message(void);
void network_progress_reset(void);
int network_has_profile(void);
void network_remember_profile(void);
void network_close(void);
extern volatile int transfer_cancel;
extern volatile unsigned transfer_kb, transfer_total_kb;
extern char network_error[160];
/* C ABI used by the C++17 Wiliwili-compatible API layer. The returned text is
 * allocated with malloc() and must be released with bili_http_free(). */
enum {
    BILI_HTTP_OK,
    BILI_HTTP_CANCELLED,
    BILI_HTTP_DISCONNECTED,
    BILI_HTTP_MEMORY,
    BILI_HTTP_TRANSPORT,
    BILI_HTTP_STATUS
};
typedef struct {
    char *text;
    int failure, transport_code, status;
    char cookies[4096];
} BiliHttpResponse;
int bili_http_credential_host(const char *url);
int bili_http_login_host(const char *url);
size_t bili_http_capture_cookie(char *data, size_t size, size_t count, void *response);
int bili_http_get_response(const char *url, BiliHttpResponse *out, size_t max_bytes);
int bili_http_post_response(const char *url, const char *form, BiliHttpResponse *out, size_t max_bytes);
/* Called only between joined jobs; credentials are scoped to API/passport HTTPS. */
void bili_http_set_session(const char *cookies);
int bili_http_get_text(const char *url,char **out,size_t max_bytes);
int bili_http_download(const char *url,const char *path,size_t max_bytes);
void bili_http_free(void *memory);
#ifdef __cplusplus
}
#endif
#endif
