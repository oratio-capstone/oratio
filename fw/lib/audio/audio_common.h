#pragma once

#include <stdint.h>

#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_SAMPLES_PER_FRAME 320 // 20 ms at 16 kHz

typedef struct
{
    uint16_t sequence;
    uint16_t length; // in samples, <= AUDIO_SAMPLES_PER_FRAME
    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
} audio_frame_t;
