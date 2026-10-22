## Audio output bring-up

The speaker/I2S wrapper and a standalone tone harness are
implemented in `lib/audio/audio_output.c/.h` and
`test/test_148_audio_output/test_main.c`. See [audio playback bring-up](docs/audio_playback.md)
for API contracts, provisional wiring, build/upload commands, host checks, and
pending hardware validation. The normal environment still runs the blink app.

## Intended Directory Structure

```text
fw/
├── lib/
│   ├── network/
│   │   ├── network.c
│   │   └── network.h
│   └── audio/
│       ├── audio_capture.c
│       ├── audio_capture.h
│       ├── audio_output.c
│       ├── audio_output.h
│       ├── audio_playback.c
│       ├── audio_playback.h
│       └── audio_common.h
├── src/
│   ├── main.c
│   └── idf_component.yml
├── test/
├── platformio.ini
└── sdkconfig.defaults
```

`audio_common.h` defines the shared PCM format (sample rate, sample type, frame duration, and derived samples/bytes per frame) used by capture, playback, and network code. Keep microphone/speaker GPIO and I²S slot/DMA settings in their respective modules: microphone input and playback output use different hardware formats.

`src/idf_component.yml` declares ESP-IDF managed dependencies, such as Mesh-Lite, when they are added.

Capture, the queued playback service, and networking shown above are planned;
their source files are not yet present on `main`. The audio component is
explicitly registered through CMake, including its ESP-IDF driver dependencies.
