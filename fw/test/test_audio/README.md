# Playback validation

Tracking: [driver #6](https://github.com/oratio-capstone/oratio/issues/6),
[driver tests #7](https://github.com/oratio-capstone/oratio/issues/7),
[task #8](https://github.com/oratio-capstone/oratio/issues/8),
[integration #9](https://github.com/oratio-capstone/oratio/issues/9).

From `fw`, compile without hardware:

```sh
pio run -e esp32-c3-devkitm-1
pio test -e audio_test --without-uploading --without-testing
```

The normal application remains the blink scaffold. The test uses real ESP-IDF,
FreeRTOS message buffers and PlatformIO Unity; it does not emulate peripherals.

Before uploading, power off and verify the MAX98357A wiring against
`audio_output.h`. The C3 mapping changed from the previous demonstration:

| Signal | C3 GPIO | S3 GPIO |
| --- | --- | --- |
| BCLK | 0 | 12 |
| WS / LRC | 1 | 13 |
| DOUT to amplifier DIN | 3 | 14 |
| SD_MODE | 7 | 11 |

Use the board's correct supply/common ground and existing speaker connections.
Ensure SD_MODE is not hard-wired to a supply before connecting its GPIO.
The S3 header marks SD_MODE as not yet routed on the PCB. S3 is not validated here.

After confirming wiring and the current serial port:

```sh
pio test -e audio_test --upload-port COM6 --test-port COM6
```

Replace COM6 with the actual port. Tests use the Super Mini's native USB console
(4 MB flash test defaults). The
normal build retains main's configuration, including its existing 2 MB warning.
Expect three Unity cases: argument/state
checks, direct output, and buffered playback. Listen for a quiet 500 Hz tone,
amplifier shutdown, then buffered tone with a one-second producer pause and
recovery. Half-length frames should retain the same pitch. Confirm no repeated
old audio during the pause. Unity success alone does not verify sound or DMA
write success in the asynchronous worker; the buffer-drain assertion verifies
consumption only. Record listening observations and serial output separately.

The playback task and its message buffer live until reboot. Do not close the
driver or delete the input buffer while the task is running. Call open before
task_start, once from startup; the playback task is the sole output writer.
No stop API, software volume, reordering, or jitter buffer is included.

One complete `audio_frame_t` is required per message, even for short audio:
`length` is 1..320 samples. Short messages/invalid lengths are discarded.
Oversized messages remain at the head of a FreeRTOS message buffer; the worker
logs once and suspends rather than spinning. Fix the producer and reboot.
Output failures are counted with rejected frames in a rate-limited warning;
frames are not retried. SD_MODE controls the amplifier without stopping I2S.

Integration checkpoint (2026-11-05): Fayza's branch at `6ae0a73` already sends
complete frames through a message buffer, nonblocking and dropping new frames
when full. However, its shared struct is samples-first and length is in bytes.
It must adopt main's metadata-first, sample-count contract and adjust codecs
before integration. Do not copy its old audio_common.h over main's header.
Use a single producer/consumer per buffer and retain it for the task lifetime.

The core-1 comment in playback.h applies to S3; the implementation uses core 0
on single-core C3. Public headers remain unchanged for team review.
Hardware tests are pending: the owner reports the board is not connected or
rewired. No previous implementation's hardware result applies to this build.
