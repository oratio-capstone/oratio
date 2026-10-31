# Microphone capture (AB#157)

## Scope and status

`lib/audio/audio_capture.c/.h` implements the INMP441 microphone driver and the
FreeRTOS capture task that feeds the network task. Host contract tests pass
(`tools/test_audio_capture.py`). The hardware validation app
(`test/test_157_audio_capture`) builds for the ESP32-C3 and must be run on the
board to establish hardware acceptance.

```text
INMP441 -> I2S RX DMA (32-bit slots) -> audio_capture_read_frame -> PCM16
        -> capture task -> audio_capture_frame_t -> message buffer -> network task
```

## Capture -> network interface (agreed Oct 29)

Every 20 ms the capture task sends one `audio_capture_frame_t` into a FreeRTOS
**message buffer** created by the network side:

| Field | Type | Meaning |
| --- | --- | --- |
| `sequence` | `uint16_t` | Set by capture. +1 per captured frame, wraps at 65535 |
| `length` | `uint16_t` | Audio bytes in `payload`: 640 for a full 20 ms frame |
| `payload` | `uint8_t[640]` | Signed 16-bit mono PCM, 16 kHz, low byte first |

These are the last three fields of the Oratio packet. The network task adds the
type (`'A'`) and node ID, and serializes the header big-endian with
`oratio_packet_encode`. Fayza's network-loop sequence counter stays for
heartbeats only.

- Only `AUDIO_CAPTURE_FRAME_HEADER_BYTES + length` bytes are sent (644 for a full
  frame), so the receiver knows the size from `xMessageBufferReceive`.
- Size the buffer with `AUDIO_CAPTURE_MESSAGE_BUFFER_BYTES(frames)`; 4 frames
  (80 ms) is the default used by the test.
- Capture **never waits** on the buffer. If it is full the frame is dropped,
  counted in `frames_dropped`, and the sequence still advances so the gap is
  visible downstream.
- Use `audio_capture_decode_pcm()` to turn the payload back into `int16_t`; never
  cast the payload pointer to `int16_t *`.

Open points for the team: the struct is named `audio_capture_frame_t` because
`audio_common.h` already uses `audio_frame_t` for a plain PCM frame, and
little-endian PCM byte order is a proposal (it matches WAV and the ESP32's native
order). Confirm both before Fayza adds them to the protocol wiki page.

## API

Singleton, task context only. One owner calls init/start/stop/deinit.

1. `audio_capture_init(&config)`: validates three distinct pins (and the optional
   status LED), allocates an I2S RX channel on controller 0 (master, Philips,
   32-bit slots, mono left, 4 DMA descriptors of 320 frames) and leaves it stopped.
2. `audio_capture_task_start(buffer)`: starts the microphone and the capture task
   (priority 5, 4 KB stack). The status LED turns on.
3. `audio_capture_task_stop()`: the task finishes its current frame and exits,
   then the microphone stops. Starting again continues the sequence. Push-to-talk
   will call start/stop.
4. `audio_capture_get_stats()`: frames captured/sent/dropped, read timeouts and
   errors, DMA overflows, next sequence and stack headroom.
5. `audio_capture_deinit()`: stops everything and releases I2S. Idempotent.

`audio_capture_start/stop/read_frame` give direct driver access for tests; do
not call them while the task is running (`stop` returns `ESP_ERR_INVALID_STATE`).

`gain_shift` adds digital gain in 6 dB steps (0-8) with saturation. The INMP441
is quiet at normal speaking distance, so tune it with the WAV recording below.

## Hardware configuration (provisional)

| INMP441 | ESP32-C3 | Notes |
| --- | --- | --- |
| VDD | 3V3 | Never 5 V |
| GND | GND | |
| L/R | GND | Left channel |
| SCK | GPIO 0 | Moved off GPIO 4: the speaker uses 4, 5, 7 |
| WS | GPIO 1 | Moved off GPIO 5 |
| SD | GPIO 6 | Unchanged |

Avoid 2, 8, 9 (strapping), 10 (PTT button), 18/19 (USB) and 20/21 (UART). Pins are
build flags in `platformio.ini`, so change them there if the PCB needs others.

## Testing

**Host contract tests** (no board needed): `python tools/test_audio_capture.py`.
They compile the real driver against fake I2S/GPIO/FreeRTOS and check
validation, lifecycle, 32-bit to 16-bit conversion, gain saturation, byte order,
frame layout, sequencing and wrap-around, drop handling, error counting and
task start/stop/rollback.

**Hardware validation**: `pio run -e audio_capture_test -t upload -t monitor`.
The app plays the network task for 10 s, prints a per-second summary
(frames, missing, peak-to-peak, RMS dBFS, DC offset), restarts capture for 1 s,
then prints PASS/FAIL for:

| Check | Pass criterion |
| --- | --- |
| Frames received | at least 98 % of 500 |
| Real-time rate | 49-51 frames/s |
| Sequence continuity | no missing or out-of-order frames |
| Frame format | every frame 640 bytes, no malformed messages |
| Capture health | 0 drops, read timeouts, read errors, DMA overflows |
| Microphone signal | peak-to-peak at least 20 (catches dead SD wire / wrong channel) |
| Stop/start | 45 or more frames after restart, sequence continues |
| Task stack | at least 512 bytes free |

**Listening test and audio metrics**: `pio run -e audio_capture_record -t upload`,
then `python tools/record_capture_wav.py --port <port> --out capture.wav`. It saves
the 10 s capture as a WAV file and reports missing frames, peak/RMS level,
DC offset, clipping, noise floor and speech-to-room dynamic range.
