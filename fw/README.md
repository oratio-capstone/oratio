## Directory Structure

```text
fw/
├── lib/
│   ├── network/
│   │   ├── network.c
│   │   └── network.h
│   └── audio/
│       ├── audio_common.h
│       ├── audio_capture.c
│       ├── audio_capture.h
│       ├── audio_output.c
│       ├── audio_output.h
│       ├── playback.c
│       └── playback.h
├── src/
│   ├── main.c
│   └── idf_component.yml
├── test/
├── platformio.ini
└── sdkconfig.defaults
```

`audio_common.h` defines the shared PCM format (sample rate, samples per frame) and `audio_frame_t`, the frame passed between capture, network, and playback through FreeRTOS message buffers.

- `audio_capture`: INMP441 driver (open/read/close) and the capture task, which sends frames to the network task.
- `audio_output`: MAX98357A driver (open/write/close, SD_MODE amp enable).
- `playback`: task that receives frames from the network task and writes them to `audio_output`.

The headers are the agreed interfaces. Don't change them without discussing it with the team first. Keep microphone/speaker GPIO and I²S slot/DMA settings in their respective modules: microphone input and playback output use different hardware formats.

`src/idf_component.yml` declares ESP-IDF managed dependencies, such as Mesh-Lite, when they are added.
