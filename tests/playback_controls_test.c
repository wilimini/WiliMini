#include "../decoder/include/playback_controls.h"
#include <assert.h>
#include <stdio.h>
static void init(PlaybackControls *s) {
    pc_init(s, 1000000, 0);
    pc_input(s, 0, 1000000);
    s->seekable = 1; s->duration_ms = 60000; s->position_ms = 20000;
}
int main(void) {
    PlaybackControls s;
    pc_init(&s, 1000000, 0);
    assert(pc_input(&s, PC_CONFIRM, 1000000) == PC_NONE && !s.paused);
    assert(pc_input(&s, PC_LEFT, 1100000) == PC_NONE && !s.seeking);
    pc_input(&s, 0, 1200000);
    pc_input(&s, PC_PAUSE, 2000000);
    assert(s.paused && pc_clock(&s, 9000000) == 2000000);
    pc_input(&s, PC_PAUSE, 8000000); assert(s.paused);
    pc_input(&s, 0, 9000000);
    pc_input(&s, PC_PAUSE, 10000000);
    assert(!s.paused && pc_clock(&s, 11000000) == 3000000);
    init(&s);
    pc_input(&s, PC_RIGHT, 1100000); assert(s.preview_ms == 21000);
    pc_input(&s, PC_RIGHT, 1200000); assert(s.preview_ms == 21000);
    pc_input(&s, PC_RIGHT, 1450000); assert(s.preview_ms == 22000);
    assert(pc_input(&s, 0, 1500000) == PC_NONE);
    assert(pc_input(&s, 0, 1900000) == PC_SEEK);
    init(&s); pc_input(&s, PC_LEFT, 1100000);
    assert(pc_input(&s, PC_BACK, 1200000) == PC_NONE && !s.seeking);
    pc_input(&s, 0, 1300000);
    assert(pc_input(&s, PC_BACK, 1400000) == PC_EXIT);
    init(&s); s.position_ms = 59000;
    pc_input(&s, PC_RIGHT, 1100000); assert(s.preview_ms == 59000);
    init(&s); s.position_ms = 1000;
    pc_input(&s, PC_LEFT, 1100000); assert(s.preview_ms == 0);
    init(&s); s.seekable = 0;
    pc_input(&s, PC_RIGHT, 1100000); assert(!s.seeking);
    init(&s); pc_input(&s, 0, 5000000); assert(!s.visible);
    pc_input(&s, PC_PAUSE, 6000000); pc_input(&s, 0, 10000000);
    assert(s.visible && s.paused);
    init(&s);
    assert(pc_input(&s, PC_FORWARD, 1100000) == PC_SEEK && s.preview_ms == 30000);
    init(&s);
    assert(pc_input(&s, PC_REWIND, 1100000) == PC_SEEK && s.preview_ms == 10000);
    init(&s); s.settings.quality_count = 3; s.settings.qualities[1] = 32; s.settings.qualities[2] = 64;
    pc_input(&s, PC_PANEL, 1100000); pc_input(&s, PC_CONFIRM, 1200000);
    pc_input(&s, PC_RIGHT, 1300000); assert(s.quality_index == 0 && !s.seeking);
    pc_input(&s, PC_UP, 1400000); assert(s.quality_index == 1 && s.settings.quality == 16);
    pc_input(&s, PC_BACK, 1500000); pc_input(&s, PC_CONFIRM, 1600000);
    assert(s.quality_index == 0); // Cancel discards preview.
    pc_input(&s, PC_UP, 1700000); assert(pc_input(&s, PC_CONFIRM, 1800000) == PC_QUALITY);
    init(&s); s.settings.subtitle_count = 2;
    pc_input(&s, PC_PANEL, 1100000); pc_input(&s, PC_LEFT, 1200000);
    pc_input(&s, PC_CONFIRM, 1300000); pc_input(&s, PC_DOWN, 1400000);
    assert(s.menu_value == 1 && s.settings.subtitle_track == 0);
    pc_input(&s, PC_BACK, 1500000); pc_input(&s, PC_CONFIRM, 1600000);
    assert(s.menu_value == 0);
    pc_input(&s, PC_DOWN, 1700000); pc_input(&s, PC_CONFIRM, 1800000);
    assert(s.settings.subtitle_track == 1 && s.menu_open == PC_FOOTER);
    pc_input(&s, PC_LEFT, 1900000); pc_input(&s, PC_CONFIRM, 2000000);
    pc_input(&s, 0, 2100000); pc_input(&s, PC_CONFIRM, 2200000);
    assert(!s.settings.danmaku_enabled && s.menu_open == PC_MENU && s.menu_row == 1);
    pc_input(&s, PC_DOWN, 2300000); pc_input(&s, PC_CONFIRM, 2400000);
    assert(s.menu_open == PC_VALUES && s.menu_value == 1);
    pc_input(&s, PC_DOWN, 2500000); assert(s.settings.danmaku_density == 1 && s.menu_value == 2);
    pc_input(&s, PC_BACK, 2600000); assert(s.menu_open == PC_MENU);
    pc_input(&s, PC_CONFIRM, 2700000); assert(s.menu_value == 1);
    pc_input(&s, PC_DOWN, 2800000); pc_input(&s, PC_CONFIRM, 2900000);
    assert(s.settings.danmaku_density == 2 && s.menu_open == PC_MENU);
    pc_input(&s, PC_DOWN, 3000000); pc_input(&s, PC_CONFIRM, 3100000);
    pc_input(&s, PC_UP, 3200000); assert(s.settings.danmaku_opacity == 75);
    pc_input(&s, PC_CONFIRM, 3300000); assert(s.settings.danmaku_opacity == 50);
    pc_input(&s, PC_PANEL, 3400000); assert(s.menu_open == PC_PLAYING);
    /* Analog stick: horizontal scrubs, vertical sets the volume, neither fights the d-pad. */
    init(&s);
    s.stick_x = 127; pc_input(&s, 0, 1100000);                 /* engage: no time has passed yet */
    assert(s.seeking && s.preview_ms == 20000);
    pc_input(&s, 0, 1200000);                                   /* 100 ms at full tilt = 6 s */
    assert(s.preview_ms == 26000 && s.seeking);
    assert(pc_input(&s, 0, 1700000) == PC_NONE);                /* still tilted: never commits */
    assert(s.preview_ms == 26000 + 6000);                       /* the 100 ms cap holds the step */
    s.stick_x = 0;
    assert(pc_input(&s, 0, 1750000) == PC_NONE && s.seeking);   /* released: wait, then commit */
    assert(pc_input(&s, 0, 2200000) == PC_SEEK);
    init(&s); s.stick_x = -50; pc_input(&s, 0, 1100000); pc_input(&s, 0, 1200000);
    assert(s.preview_ms < 20000 && s.preview_ms > 18000);       /* a half tilt back is slower than full (6 s) */
    init(&s); s.position_ms = 500; s.stick_x = -127; pc_input(&s, 0, 1100000); pc_input(&s, 0, 1200000);
    assert(s.preview_ms == 0);
    init(&s); s.seekable = 0; s.stick_x = 127; pc_input(&s, 0, 1100000); pc_input(&s, 0, 1200000);
    assert(!s.seeking);
    init(&s); s.stick_x = 127; pc_input(&s, PC_LEFT, 1100000); pc_input(&s, PC_LEFT, 1200000);
    assert(s.preview_ms == 19000);                              /* the d-pad takes priority */
    init(&s); s.stick_y = -127; pc_input(&s, 0, 1100000);
    assert(s.settings.volume_cut == 0);
    pc_input(&s, 0, 1200000); pc_input(&s, 0, 1300000); pc_input(&s, 0, 1400000);
    assert(s.settings.volume_cut == 0 && !s.seeking);           /* already at full volume */
    s.stick_y = 127; pc_input(&s, 0, 1500000);
    for (int64_t t = 1600000; t <= 2600000; t += 100000) pc_input(&s, 0, t);
    assert(s.settings.volume_cut >= 46 && s.settings.volume_cut <= 50 && s.volume_flash_until > 2600000);
    assert(s.visible && !s.seeking);
    s.stick_y = -127;
    for (int64_t t = 2700000; t <= 5700000; t += 100000) pc_input(&s, 0, t);
    assert(s.settings.volume_cut == 0);                         /* clamps at full volume */
    s.stick_y = 127;
    for (int64_t t = 5800000; t <= 12000000; t += 100000) pc_input(&s, 0, t);
    assert(s.settings.volume_cut == 100);                       /* and at silence */
    init(&s); s.stick_y = 127; s.stick_x = 60;                  /* diagonal: the stronger axis wins */
    pc_input(&s, 0, 1100000); pc_input(&s, 0, 1200000);
    assert(!s.seeking && s.settings.volume_cut > 0);
    init(&s); pc_input(&s, PC_PANEL, 1100000); s.stick_y = 127; pc_input(&s, 0, 1200000); pc_input(&s, 0, 1300000);
    assert(s.settings.volume_cut == 0);                         /* menus ignore the stick */
    puts("playback controls: PASS");
}
