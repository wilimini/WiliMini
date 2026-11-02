#ifndef WILI_ANALOG_STICK_H
#define WILI_ANALOG_STICK_H
/* Platform-independent analog stick helpers shared by the client UI, the on-screen keyboard and
 * the player (C and C++). The PSP reports each axis as an unsigned byte with the rest position
 * near 128. Real nubs drift, so a wide dead zone is applied before anything reacts, and the
 * stick only adds to the d-pad: every action it triggers is reachable without it. */
#include <stdint.h>

enum { STICK_CENTER = 128, STICK_DEADZONE = 36, STICK_MAX = 127 };
enum { STICK_NONE, STICK_LEFT, STICK_RIGHT, STICK_UP, STICK_DOWN };

/* Raw axis byte -> -127..127. Zero inside the dead zone; the rest of the travel is stretched
 * so the value still reaches the extremes. */
static inline int stick_deflection(unsigned char raw) {
    int d = (int)raw - STICK_CENTER;
    const int sign = d < 0 ? -1 : 1;
    if (d < 0) d = -d;
    if (d <= STICK_DEADZONE) return 0;
    d = (d - STICK_DEADZONE) * STICK_MAX / (STICK_MAX - STICK_DEADZONE);
    if (d > STICK_MAX) d = STICK_MAX;
    return d ? sign * d : 0;
}

/* Steps per second (thousandths) for a deflection of 0..127: a gentle tilt walks slowly, a
 * full tilt runs at about the d-pad's held-repeat rate (min..max in milli-steps). */
static inline int64_t stick_rate_milli(int deflection, int min_milli, int max_milli) {
    if (deflection < 0) deflection = -deflection;
    if (deflection > STICK_MAX) deflection = STICK_MAX;
    return min_milli + (int64_t)(max_milli - min_milli) * deflection * deflection / (STICK_MAX * STICK_MAX);
}

/* Turns a held stick into discrete up/down/left/right steps for list and cursor movement. The
 * dominant axis wins so a diagonal never moves both ways. A direction change or a fresh tilt
 * steps once at once, then continues at a rate that grows with the tilt. */
typedef struct {
    int direction;       /* STICK_* currently held, STICK_NONE at rest */
    int64_t last_us;
    int64_t progress;    /* milli-steps scaled by 1e6, so a step is 1e9 */
} StickNav;

static inline void stick_nav_reset(StickNav *s) { s->direction = STICK_NONE; s->last_us = 0; s->progress = 0; }

/* Returns the number of steps (0..3) to take in *direction this call. horizontal=0 ignores the
 * x axis for screens where left/right has another meaning (value rows, like buttons). */
static inline int stick_nav_update(StickNav *s, unsigned char x, unsigned char y, int horizontal,
                                   int min_milli, int max_milli, int64_t now_us, int *direction) {
    const int dx = horizontal ? stick_deflection(x) : 0, dy = stick_deflection(y);
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int dir = STICK_NONE, depth = 0;
    if (ax || ay) {
        /* Hysteresis: stay on the current axis until the other clearly dominates. */
        const int held_horizontal = s->direction == STICK_LEFT || s->direction == STICK_RIGHT;
        const int use_x = s->direction == STICK_NONE ? ax > ay
                        : held_horizontal ? ax * 2 >= ay : ax > ay * 2;
        if (use_x) { dir = dx < 0 ? STICK_LEFT : STICK_RIGHT; depth = ax; }
        else { dir = dy < 0 ? STICK_UP : STICK_DOWN; depth = ay; }
    }
    *direction = dir;
    if (dir == STICK_NONE) { stick_nav_reset(s); return 0; }
    int64_t dt = s->last_us ? now_us - s->last_us : 0;
    s->last_us = now_us;
    if (dt < 0) dt = 0;
    if (dt > 100000) dt = 100000; /* a stalled frame must not become a burst of steps */
    int steps = 0;
    if (dir != s->direction) { s->direction = dir; s->progress = 0; steps = 1; }
    s->progress += stick_rate_milli(depth, min_milli, max_milli) * dt;
    while (s->progress >= 1000000000LL && steps < 3) { s->progress -= 1000000000LL; ++steps; }
    if (s->progress > 1000000000LL) s->progress = 1000000000LL;
    return steps;
}

/* Scrubbing speed for the player: milliseconds of video per second of stick time, from a slow
 * creep to one minute per second at full tilt. */
static inline int64_t stick_scrub_ms_per_second(int deflection) {
    return stick_rate_milli(deflection, 2000, 60000);
}

#endif
