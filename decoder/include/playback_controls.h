#ifndef WILI_PLAYBACK_CONTROLS_H
#define WILI_PLAYBACK_CONTROLS_H
#include <stdint.h>
#include "psp_video.h"
#include <string.h>

/* Platform-independent input and virtual clock. Caller serializes access. */
enum { PC_CONFIRM=1, PC_BACK=2, PC_PAUSE=4, PC_PANEL=8, PC_LEFT=16, PC_RIGHT=32, PC_REWIND=64, PC_FORWARD=128, PC_UP=256, PC_DOWN=512, PC_DANMAKU=1024 };
enum { PC_NONE, PC_EXIT, PC_SEEK, PC_QUALITY };
typedef struct {
    unsigned previous, direction;
    int paused, visible, seeking, seekable, armed;
    int menu_open, menu_row, quality_index;
    PspVideoSettings settings;
    int64_t position_ms, duration_ms, preview_ms;
    int64_t paused_at, paused_total, hide_at, repeat_at, commit_at;
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
        if (s->menu_open) { s->menu_open = s->menu_open == 2 ? 1 : 0; s->hide_at = now + 3000000; return PC_NONE; }
        if (!s->seeking) return PC_EXIT;
        s->seeking = 0; s->commit_at = 0; s->direction = direction;
        s->hide_at = now + 3000000;
        return PC_NONE;
    }
    if (pressed & PC_PANEL) {
        s->menu_open = !s->menu_open;
        if (s->menu_open) { s->seeking = 0; s->commit_at = 0; s->direction = 0; }
        s->visible = 1;
        s->hide_at = now + 3000000;
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
        if (pressed & PC_CONFIRM) s->menu_open = 2;
        if (pressed & PC_PAUSE) pc_pause(s, now);
        s->direction = 0;
        return PC_NONE;
    }
    if (s->menu_open == 2) {
        if (s->menu_row >= 1 && s->menu_row <= 3) {
            if (pressed & PC_UP) s->menu_row = 1 + (s->menu_row + 1) % 3;
            if (pressed & PC_DOWN) s->menu_row = 1 + s->menu_row % 3;
        }
        int change = (pressed & PC_LEFT) ? -1 : (pressed & PC_RIGHT) ? 1 : 0;
        if (s->menu_row == 0) {
            if (pressed & PC_UP) change = 1;
            if (pressed & PC_DOWN) change = -1;
            if (change && s->settings.quality_count > 0)
                s->quality_index = (s->quality_index + change + s->settings.quality_count) % s->settings.quality_count;
            if (pressed & PC_CONFIRM) {
                if (s->settings.quality_count > 0 && s->settings.qualities[s->quality_index] != s->settings.quality)
                    return PC_QUALITY;
                s->menu_open = 1;
            }
        } else if (s->menu_row == 4) {
            if (pressed & PC_UP) change = -1;
            if (pressed & PC_DOWN) change = 1;
            const int count = s->settings.subtitle_count + 1;
            if (change) s->settings.subtitle_track = (s->settings.subtitle_track + change + count) % count;
            if (pressed & PC_CONFIRM) s->menu_open = 1;
        } else {
            if (pressed & PC_CONFIRM) change = 1;
            if (change) {
                if (s->menu_row == 1) s->settings.danmaku_enabled = !s->settings.danmaku_enabled;
                else if (s->menu_row == 2) s->settings.danmaku_density = (s->settings.danmaku_density + change + 3) % 3;
                else s->settings.danmaku_opacity = 50 + ((s->settings.danmaku_opacity / 25 - 2 + change + 3) % 3) * 25;
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
    if (s->seekable && (direction == PC_LEFT || direction == PC_RIGHT)) {
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
    } else if (s->seeking) {
        if (!s->commit_at) s->commit_at = now + 400000;
        if (now >= s->commit_at) return PC_SEEK;
    }
    s->direction = direction;
    if (!s->paused && !s->seeking && !s->menu_open && now >= s->hide_at) s->visible = 0;
    return PC_NONE;
}
#endif
