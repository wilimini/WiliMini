/* Muxed FLV transport used by the Wiliwili live-room flow. Video and audio
 * demuxers read the same bounded buffer through independent cursors. */
#include "bilibili.h"
#include <curl/curl.h>
#include <pspkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_CAPACITY (2 * 1024 * 1024)
#define LIVE_LOOKBACK (64 * 1024)
struct BiliLiveStream;
typedef struct {
  struct BiliLiveStream *stream;
  int index;
} LiveReader;
struct BiliLiveStream {
  unsigned char *ring;
  char *url;
  uint64_t written, position[2];
  volatile int cancelled, ended, failed;
  SceUID lock, thread;
  LiveReader readers[2];
  PspVideoSource sources[2];
};
static int live_cancelled(BiliLiveStream *s) {
  return s->cancelled || transfer_cancel;
}
static size_t live_write(char *data, size_t size, size_t count, void *opaque) {
  BiliLiveStream *s = opaque;
  size_t total = size * count, done = 0;
  while (done < total && !live_cancelled(s)) {
    sceKernelWaitSema(s->lock, 1, NULL);
    uint64_t minimum =
        s->position[0] < s->position[1] ? s->position[0] : s->position[1];
    uint64_t retained = minimum > LIVE_LOOKBACK ? minimum - LIVE_LOOKBACK : 0;
    size_t free_bytes = LIVE_CAPACITY - (size_t)(s->written - retained);
    size_t part = total - done;
    size_t at = (size_t)(s->written % LIVE_CAPACITY);
    if (part > free_bytes)
      part = free_bytes;
    if (part > LIVE_CAPACITY - at)
      part = LIVE_CAPACITY - at;
    if (part) {
      memcpy(s->ring + at, data + done, part);
      s->written += part;
      done += part;
    }
    sceKernelSignalSema(s->lock, 1);
    if (!part)
      sceKernelDelayThread(10000);
  }
  return done;
}
static int live_progress(void *opaque, curl_off_t a, curl_off_t b, curl_off_t c,
                         curl_off_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return live_cancelled(opaque);
}
static int live_download(SceSize args, void *opaque) {
  (void)args;
  BiliLiveStream *s = *(BiliLiveStream **)opaque;
  CURL *curl = curl_easy_init();
  if (!curl) {
    s->failed = s->ended = 1;
    return 0;
  }
  curl_easy_setopt(curl, CURLOPT_URL, s->url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "wiliwili");
  curl_easy_setopt(curl, CURLOPT_REFERER, "https://live.bilibili.com/");
  curl_easy_setopt(curl, CURLOPT_CAINFO, CLIENT_PATH("/cacert.pem"));
  curl_easy_setopt(curl, CURLOPT_CAPATH, (char *)NULL);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, live_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, s);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, live_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, s);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  s->failed = !live_cancelled(s) && (result != CURLE_OK || status != 200);
  __asm__ volatile("" ::: "memory");
  s->ended = 1;
  return 0;
}
static int live_read(void *opaque, int64_t offset, void *buffer, int bytes) {
  LiveReader *reader = opaque;
  BiliLiveStream *s = reader->stream;
  if (offset < 0 || bytes < 0)
    return -1;
  if (!bytes)
    return 0;
  for (;;) {
    if (live_cancelled(s))
      return -1;
    sceKernelWaitSema(s->lock, 1, NULL);
    const uint64_t at = (uint64_t)offset;
    if (at + LIVE_CAPACITY < s->written) {
      sceKernelSignalSema(s->lock, 1);
      return -1;
    }
    if (at < s->written) {
      size_t count = (size_t)(s->written - at),
             index = (size_t)(at % LIVE_CAPACITY);
      if (count > (size_t)bytes)
        count = (size_t)bytes;
      if (count > LIVE_CAPACITY - index)
        count = LIVE_CAPACITY - index;
      memcpy(buffer, s->ring + index, count);
      s->position[reader->index] = at + count;
      sceKernelSignalSema(s->lock, 1);
      return (int)count;
    }
    int ended = s->ended, failed = s->failed;
    sceKernelSignalSema(s->lock, 1);
    if (ended)
      return failed ? -1 : 0;
    sceKernelDelayThread(10000);
  }
}
static void live_cancel(void *opaque) {
  LiveReader *reader = opaque;
  reader->stream->cancelled = 1;
}
const PspVideoSource *bili_live_source(BiliLiveStream *s, int audio) {
  return s ? &s->sources[audio ? 1 : 0] : NULL;
}
int bili_live_finish(BiliLiveStream *s) {
  if (!s)
    return 0;
  s->cancelled = 1;
  if (s->thread >= 0) {
    sceKernelWaitThreadEnd(s->thread, NULL);
    sceKernelDeleteThread(s->thread);
  }
  int failed = s->failed;
  if (s->lock >= 0)
    sceKernelDeleteSema(s->lock);
  free(s->url);
  free(s->ring);
  free(s);
  return failed ? -1 : 0;
}
int bili_prepare_live_playback(const char *url, BiliPlayback *out) {
  if (!url || !out ||
      (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)))
    return -1;
  if (network_connect() < 0)
    return -1;
  BiliLiveStream *s = calloc(1, sizeof(*s));
  if (!s)
    return -1;
  s->lock = s->thread = -1;
  s->ring = malloc(LIVE_CAPACITY);
  s->url = strdup(url);
  if (!s->ring || !s->url) {
    bili_live_finish(s);
    return -1;
  }
  s->lock = sceKernelCreateSema("live_ring", 0, 1, 1, NULL);
  if (s->lock < 0) {
    bili_live_finish(s);
    return -1;
  }
  for (int i = 0; i < 2; i++) {
    s->readers[i] = (LiveReader){s, i};
    s->sources[i] =
        (PspVideoSource){&s->readers[i], -1, live_read, live_cancel, NULL};
  }
  s->thread = sceKernelCreateThread("live_download", live_download, 0x20,
                                    128 * 1024, PSP_THREAD_ATTR_USER, NULL);
  if (s->thread < 0) {
    bili_live_finish(s);
    return -1;
  }
  if (sceKernelStartThread(s->thread, sizeof(s), &s) < 0) {
    sceKernelDeleteThread(s->thread);
    s->thread = -1;
    bili_live_finish(s);
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->streaming = 1;
  out->live = s;
  snprintf(out->path, sizeof(out->path), "live.flv");
  return 0;
}
