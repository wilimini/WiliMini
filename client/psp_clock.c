/* Firmware libc time can expose seconds since midnight under PSPLink. Use
 * the RTC epoch for libc consumers, including mbedTLS certificate validity. */
#include <psprtc.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

static int utc_time(struct timeval *tv) {
    u64 tick;
    const u64 unix_epoch=62135596800000000ULL;
    if(sceRtcGetCurrentTick(&tick)<0 || tick<unix_epoch){errno=EINVAL;return -1;}
    tick-=unix_epoch;
    tv->tv_sec=(time_t)(tick/1000000);
    tv->tv_usec=(suseconds_t)(tick%1000000);
    return 0;
}
int __wrap_gettimeofday(struct timeval *tv,void *zone) {
    (void)zone;
    if(!tv){errno=EINVAL;return -1;}
    return utc_time(tv);
}
time_t __wrap_time(time_t *out) {
    struct timeval tv;
    time_t value=utc_time(&tv)<0?(time_t)-1:tv.tv_sec;
    if(out)*out=value;
    return value;
}
