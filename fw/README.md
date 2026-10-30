## Audio output bring-up

The queued playback service is documented in [Audio playback library](docs/audio_playback_service.md),
including its producer API, lifecycle, overflow policy, and software checks.

Run prerecorded speech validation with `audio_playback_test`; see
[Audio playback test](docs/audio_playback_test.md) for commands and hardware results.

The speaker/I2S wrapper and a standalone tone harness are
implemented in `lib/audio/audio_output.c/.h` and
`test/test_148_audio_output/test_main.c`. See [audio playback bring-up](docs/audio_playback.md)
for API contracts, wiring, build/upload commands, and host checks. The normal environment still runs the blink app.

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

The queued playback service is implemented on this feature branch.
Capture and networking shown above remain separate work. The audio component is
explicitly registered through CMake, including its ESP-IDF driver dependencies.
