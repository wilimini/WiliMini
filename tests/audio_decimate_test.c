#include "../decoder/src/audio_decimate.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(audio_decimation_factor(44100) == 1 && audio_decimation_factor(48000) == 1);
    assert(audio_decimation_factor(88200) == 2 && audio_decimation_factor(96000) == 2);
    assert(audio_decimation_factor(176400) == 4 && audio_decimation_factor(192000) == 4);
    assert(audio_decimation_factor(32000) == 0 && audio_decimation_factor(0) == 0 && audio_decimation_factor(384000) == 0);
    AudioDecimator d; int16_t out[2];
    audio_decimator_init(&d, 1);
    assert(audio_decimator_push(&d, -5, 7, out) == 1 && out[0] == -5 && out[1] == 7);
    // Factor 2: one output per two inputs, the average of each channel.
    audio_decimator_init(&d, 2);
    assert(audio_decimator_push(&d, 100, -100, out) == 0);
    assert(audio_decimator_push(&d, 300, -300, out) == 1 && out[0] == 200 && out[1] == -200);
    // The phase carries across calls (frames), and the sum restarts for each output.
    assert(audio_decimator_push(&d, 10, 10, out) == 0);
    assert(audio_decimator_push(&d, 20, 30, out) == 1 && out[0] == 15 && out[1] == 20);
    // Factor 4 and full scale never overflow.
    audio_decimator_init(&d, 4);
    for (int i = 0; i < 3; ++i) assert(audio_decimator_push(&d, 32767, -32768, out) == 0);
    assert(audio_decimator_push(&d, 32767, -32768, out) == 1 && out[0] == 32767 && out[1] == -32768);
    // Output count: n inputs give n / factor outputs.
    audio_decimator_init(&d, 4);
    int made = 0;
    for (int i = 0; i < 1001; ++i) made += audio_decimator_push(&d, i, i, out);
    assert(made == 250);
    // A reset (seek) drops a partial group.
    audio_decimator_init(&d, 2);
    audio_decimator_push(&d, 1000, 1000, out);
    audio_decimator_init(&d, 2);
    assert(audio_decimator_push(&d, 4, 4, out) == 0 && audio_decimator_push(&d, 6, 6, out) == 1 && out[0] == 5);
    puts("audio decimate: PASS");
}
