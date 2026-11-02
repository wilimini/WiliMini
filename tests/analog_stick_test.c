#include "../decoder/include/analog_stick.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* Rest, drift and the extremes. */
    assert(stick_deflection(128) == 0 && stick_deflection(128 + STICK_DEADZONE) == 0);
    assert(stick_deflection(128 - STICK_DEADZONE) == 0 && stick_deflection(110) == 0);
    assert(stick_deflection(255) == 127 && stick_deflection(0) == -127);
    assert(stick_deflection(128 + STICK_DEADZONE + 1) >= 0);
    int last = 0;
    for (int raw = 128; raw <= 255; ++raw) {
        int d = stick_deflection((unsigned char)raw);
        assert(d >= last && d <= STICK_MAX && stick_deflection((unsigned char)(256 - raw)) <= 0);
        last = d;
    }

    /* A gentle tilt is slower than a full one. */
    assert(stick_rate_milli(1, 3000, 11000) < stick_rate_milli(64, 3000, 11000));
    assert(stick_rate_milli(64, 3000, 11000) < stick_rate_milli(127, 3000, 11000));
    assert(stick_rate_milli(127, 3000, 11000) == 11000 && stick_rate_milli(-127, 3000, 11000) == 11000);

    /* Navigation: nothing at rest, one step on engaging, then a steady rate. */
    StickNav nav; stick_nav_reset(&nav);
    int dir = -1;
    assert(stick_nav_update(&nav, 128, 128, 1, 3000, 11000, 1000000, &dir) == 0 && dir == STICK_NONE);
    assert(stick_nav_update(&nav, 128, 255, 1, 3000, 11000, 1016000, &dir) == 1 && dir == STICK_DOWN);
    int steps = 0;
    for (int64_t t = 1032000; t <= 2016000; t += 16000) steps += stick_nav_update(&nav, 128, 255, 1, 3000, 11000, t, &dir);
    assert(steps >= 9 && steps <= 12); /* ~11 steps per second at full tilt */
    stick_nav_reset(&nav);
    stick_nav_update(&nav, 128, 255, 1, 3000, 11000, 0, &dir);
    steps = 0;
    for (int64_t t = 16000; t <= 1000000; t += 16000) steps += stick_nav_update(&nav, 128, 170, 1, 3000, 11000, t, &dir);
    assert(steps >= 1 && steps <= 5); /* a light tilt walks slowly */

    /* The dominant axis wins; horizontal can be switched off. */
    stick_nav_reset(&nav);
    assert(stick_nav_update(&nav, 255, 190, 1, 3000, 11000, 0, &dir) == 1 && dir == STICK_RIGHT);
    stick_nav_reset(&nav);
    assert(stick_nav_update(&nav, 255, 190, 0, 3000, 11000, 0, &dir) == 1 && dir == STICK_DOWN);
    stick_nav_reset(&nav);
    assert(stick_nav_update(&nav, 255, 128, 0, 3000, 11000, 0, &dir) == 0 && dir == STICK_NONE);
    /* A direction change steps at once. */
    stick_nav_reset(&nav);
    stick_nav_update(&nav, 128, 255, 1, 3000, 11000, 0, &dir);
    assert(stick_nav_update(&nav, 128, 0, 1, 3000, 11000, 16000, &dir) == 1 && dir == STICK_UP);
    /* A stalled frame cannot become a burst. */
    stick_nav_reset(&nav);
    stick_nav_update(&nav, 128, 255, 1, 3000, 11000, 0, &dir);
    assert(stick_nav_update(&nav, 128, 255, 1, 3000, 11000, 60000000, &dir) <= 3);
    /* Letting go and tilting again steps again. */
    stick_nav_update(&nav, 128, 128, 1, 3000, 11000, 60016000, &dir);
    assert(stick_nav_update(&nav, 128, 255, 1, 3000, 11000, 60032000, &dir) == 1);
    puts("analog stick: PASS");
    return 0;
}
