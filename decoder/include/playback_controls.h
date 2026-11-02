#ifndef WILI_PLAYBACK_CONTROLS_H
#define WILI_PLAYBACK_CONTROLS_H
#include <stdint.h>
#include "psp_video.h"
#include "analog_stick.h"
#include <string.h>

/* Platform-independent input and virtual clock. Caller serializes access. */
enum { PC_CONFIRM=1, PC_BACK=2, PC_PAUSE=4, PC_PANEL=8, PC_LEFT=16, PC_RIGHT=32, PC_REWIND=64, PC_FORWARD=128, PC_UP=256, PC_DOWN=512, PC_DANMAKU=1024 };
enum { PC_NONE, PC_EXIT, PC_SEEK, PC_QUALITY };
enum { PC_PLAYING, PC_FOOTER, PC_MENU, PC_VALUES };
typedef struct {
    unsigned previous, direction;
    int paused, visible, seeking, seekable, armed;
    int menu_open, menu_row, quality_index, menu_value;
    PspVideoSettings settings;
    int64_t position_ms, duration_ms, preview_ms;
    int64_t paused_at, paused_total, hide_at, repeat_at, commit_at;
    /* Analog stick deflection (-127..127) set by the caller before pc_input. Horizontal scrubs
     * the video, vertical changes the volume; both only add to the d-pad. */
    int stick_x, stick_y;
    int64_t stick_at, volume_progress, volume_flash_until;
} PlaybackControls;
static inline int64_t pc_clock(const PlaybackControls *s, int64_t now) {
    return (s->paused ? s->paused_at : now) - s->paused_total;
}
static inline void pc_pause(PlaybackControls *s, int64_t now) {
    if (s->paused) s->paused_total += now - s->paused_at;
    else s->paused_at = now;
    s->paused = !s->paused;
}
static inline void pc_init(PlaybackControls *s, int64_t now, int paused) {
    memset(s, 0, sizeof(*s));
    s->previous = ~0u; /* Ignore the button that launched playback. */
    s->visible = 1;
    s->settings.quality = s->settings.qualities[0] = 16;
    s->settings.quality_count = 1;
    s->settings.danmaku_enabled = 1;
    s->settings.danmaku_density = 1;
    s->settings.danmaku_opacity = 75;
    s->hide_at = now + 3000000;
    if (paused) pc_pause(s, now);
}
static inline int pc_input(PlaybackControls *s, unsigned buttons, int64_t now) {
    if (!s->armed) {
        s->previous = buttons;
        if (!buttons) s->armed = 1;
        return PC_NONE;
    }
    unsigned pressed = buttons & ~s->previous;
    s->previous = buttons;
    unsigned direction = buttons & (PC_LEFT | PC_RIGHT);
    if (pressed & PC_BACK) {
        if (s->menu_open) { s->menu_open--; s->visible = s->menu_open != PC_PLAYING; s->hide_at = now + 3000000; return PC_NONE; }
        if (!s->seeking) return PC_EXIT;
        s->seeking = 0; s->commit_at = 0; s->direction = direction;
        s->hide_at = now + 3000000;
        return PC_NONE;
    }
    if (pressed & PC_PANEL) {
        s->menu_open = !s->menu_open;
        if (s->menu_open) { s->seeking = 0; s->commit_at = 0; s->direction = 0; }
        s->visible = s->menu_open != PC_PLAYING;
        s->hide_at = now + 3000000;
        return PC_NONE;
    }
    if (pressed & PC_DANMAKU) {
        s->settings.danmaku_enabled = !s->settings.danmaku_enabled;
        s->visible = 1; s->hide_at = now + 3000000;
    }
    if (s->menu_open == 1) {
        // Footer order: danmaku, subtitles, quality. Never seek while focused.
        int index = s->menu_row == 0 ? 2 : s->menu_row == 4 ? 1 : 0;
        if (pressed & PC_LEFT) index = (index + 2) % 3;
        if (pressed & PC_RIGHT) index = (index + 1) % 3;
        s->menu_row = index == 2 ? 0 : index == 1 ? 4 : 1;
        if (pressed & PC_CONFIRM) {
            s->menu_open = PC_MENU;
            s->menu_value = s->settings.subtitle_track;
            for (int i = 0; i < s->settings.quality_count; ++i)
                if (s->settings.qualities[i] == s->settings.quality) s->quality_index = i;
        }
        if (pressed & PC_PAUSE) pc_pause(s, now);
        s->direction = 0;
        return PC_NONE;
    }
    if (s->menu_open >= PC_MENU) {
        const int step = (pressed & PC_UP) ? -1 : (pressed & PC_DOWN) ? 1 : 0;
        if (s->menu_open == PC_VALUES) {
            s->menu_value = (s->menu_value + step + 3) % 3;
            if (pressed & PC_CONFIRM) {
                if (s->menu_row == 2) s->settings.danmaku_density = s->menu_value;
                else s->settings.danmaku_opacity = 50 + s->menu_value * 25;
                s->menu_open = PC_MENU;
            }
        } else if (s->menu_row == 0) {
            const int count = s->settings.quality_count;
            if (count > 0) s->quality_index = (s->quality_index - step + count) % count;
            if (pressed & PC_CONFIRM) {
                if (count > 0 && s->settings.qualities[s->quality_index] != s->settings.quality) return PC_QUALITY;
                s->menu_open = PC_FOOTER;
            }
        } else if (s->menu_row == 4) {
            const int count = s->settings.subtitle_count + 1;
            if (s->menu_value >= count) s->menu_value = s->settings.subtitle_track;
            s->menu_value = (s->menu_value + step + count) % count;
            if (pressed & PC_CONFIRM) {
                s->settings.subtitle_track = s->menu_value;
                s->menu_open = PC_FOOTER;
            }
        } else {
            s->menu_row = 1 + (s->menu_row - 1 + step + 3) % 3;
            if (pressed & PC_CONFIRM) {
                if (s->menu_row == 1) s->settings.danmaku_enabled = !s->settings.danmaku_enabled;
                else {
                    s->menu_value = s->menu_row == 2 ? s->settings.danmaku_density : s->settings.danmaku_opacity / 25 - 2;
                    s->menu_open = PC_VALUES;
                }
            }
        }
        if (pressed & PC_PAUSE) pc_pause(s, now);
        s->direction = 0;
        return PC_NONE;
    }
    if (pressed & (PC_CONFIRM | PC_PAUSE)) {
        if (s->seeking) return PC_SEEK;
        pc_pause(s, now);
        s->visible = 1; s->hide_at = now + 3000000;
    }
    if (s->seekable && (pressed & (PC_REWIND | PC_FORWARD))) {
        s->preview_ms = (s->seeking ? s->preview_ms : s->position_ms) +
            ((pressed & PC_REWIND) ? -10000 : 10000);
        if (s->preview_ms < 0) s->preview_ms = 0;
        if (s->duration_ms > 0 && s->preview_ms >= s->duration_ms)
            s->preview_ms = s->duration_ms > 1000 ? s->duration_ms - 1000 : 0;
        return PC_SEEK;
    }
    const int ax = s->stick_x < 0 ? -s->stick_x : s->stick_x, ay = s->stick_y < 0 ? -s->stick_y : s->stick_y;
    int64_t stick_dt = s->stick_at ? now - s->stick_at : 0;
    if (stick_dt < 0) stick_dt = 0;
    if (stick_dt > 100000) stick_dt = 100000; /* a stalled frame must not jump the video */
    const int scrubbing = !direction && s->seekable && ax && ax > ay;
    if (!direction && ay && ay >= ax) {
        /* Up is louder. The change accumulates so a gentle tilt moves a percent at a time. */
        const int sign = s->stick_y < 0 ? 1 : -1;
        if (s->volume_progress && (s->volume_progress < 0) != (sign < 0)) s->volume_progress = 0;
        s->volume_progress += (int64_t)sign * stick_rate_milli(ay, 8000, 40000) * stick_dt;
        while (s->volume_progress >= 1000000000LL || s->volume_progress <= -1000000000LL) {
            const int step = s->volume_progress > 0 ? 1 : -1;
            s->volume_progress -= step * 1000000000LL;
            s->settings.volume_cut -= step;
            if (s->settings.volume_cut < 0) s->settings.volume_cut = 0;
            if (s->settings.volume_cut > 100) s->settings.volume_cut = 100;
            s->volume_flash_until = now + 1500000;
            s->visible = 1; s->hide_at = now + 3000000;
        }
    } else s->volume_progress = 0;
    if (scrubbing) {
        if (!s->seeking) s->preview_ms = s->position_ms;
        s->preview_ms += (int64_t)(s->stick_x < 0 ? -1 : 1) * stick_scrub_ms_per_second(ax) * stick_dt / 1000000;
        if (s->preview_ms < 0) s->preview_ms = 0;
        if (s->duration_ms > 0 && s->preview_ms >= s->duration_ms)
            s->preview_ms = s->duration_ms > 1000 ? s->duration_ms - 1000 : 0;
        s->seeking = s->visible = 1;
        s->commit_at = 0;
    }
    s->stick_at = ax || ay ? now : 0;
    if (!scrubbing && s->seekable && (direction == PC_LEFT || direction == PC_RIGHT)) {
        if (direction != s->direction || now >= s->repeat_at) {
            if (!s->seeking) s->preview_ms = s->position_ms;
            s->preview_ms += direction == PC_LEFT ? -1000 : 1000;
            if (s->preview_ms < 0) s->preview_ms = 0;
            if (s->duration_ms > 0 && s->preview_ms >= s->duration_ms)
                s->preview_ms = s->duration_ms > 1000 ? s->duration_ms - 1000 : 0;
            s->repeat_at = now + (direction != s->direction ? 350000 : 150000);
            s->seeking = s->visible = 1;
        }
        s->commit_at = 0;
    } else if (s->seeking && !scrubbing) {
        if (!s->commit_at) s->commit_at = now + 400000;
        if (now >= s->commit_at) return PC_SEEK;
    }
    s->direction = direction;
    if (!s->paused && !s->seeking && !s->menu_open && now >= s->hide_at) s->visible = 0;
    return PC_NONE;
}
#endif
