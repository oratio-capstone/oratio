## Directory Structure

```text
fw/
├── lib/
│   ├── network/
│   │   ├── network.c
│   │   └── network.h
│   └── audio/
│       ├── audio_capture.c
│       ├── audio_capture.h
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
