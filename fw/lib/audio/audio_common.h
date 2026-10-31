#pragma once

#include <stdint.h>

/* Application PCM format, independent of microphone/speaker I2S slot widths. */
enum
{
    AUDIO_SAMPLE_RATE_HZ = 16000,
    AUDIO_CHANNELS = 1,
    AUDIO_SAMPLE_BITS = 16,
    AUDIO_FRAME_DURATION_MS = 20,
    AUDIO_SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE_HZ * AUDIO_FRAME_DURATION_MS / 1000,
    AUDIO_FRAME_BYTES = AUDIO_SAMPLES_PER_FRAME * AUDIO_CHANNELS * AUDIO_SAMPLE_BITS / 8
};

typedef struct
{
    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
} audio_frame_t;

_Static_assert(AUDIO_SAMPLE_RATE_HZ *AUDIO_FRAME_DURATION_MS % 1000 == 0, "Frames require whole samples");
_Static_assert(sizeof(audio_frame_t) == AUDIO_FRAME_BYTES, "Unexpected PCM frame layout");
