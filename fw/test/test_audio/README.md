# Playback validation

## S3 N8R8 speaker-only branch

Branch: `mohamed/feature/160-s3-audio-playback`, based on playback commit `83eee37`.
This branch adds only S3 speaker test configuration. No microphone implementation,
capture task, PTT button, or loopback application is included.

Use the board's native USB connector (often labelled USB/OTG), not its UART bridge.
S3 settings: 8 MB quad flash, dual core, native USB Serial/JTAG console. The module's
8 MB octal PSRAM is intentionally unused; speaker playback fits in internal RAM.
The existing target macros select I2S1 and S3 GPIOs; playback uses core 1 on dual-core
S3. C3 uses I2S0 and core 0. Separate binaries are required; COM numbers do not select
the chip. The uploader must target the environment matching the connected board.

For S3, wire MAX98357A BCLK=GPIO42, LRC/WS=GPIO2, DIN=GPIO1, SD_MODE=GPIO47.
Use common ground and your established compatible amplifier supply/speaker wiring.
Disconnect power before rewiring; SD_MODE must not also be tied directly to a supply.
These are Adam's proposed PCB pins; hardware validation is still pending.

```powershell
cd C:\Users\moham\Documents\ChatGPT\Oratio\oratio-playback-s3\fw
$pio = 'C:\Users\moham\.platformio\penv\Scripts\pio.exe'
& $pio device list
$boardPort = 'COM12' # Replace with the actual USB device port, not Bluetooth.
& $pio test -e audio_test_s3 --upload-port $boardPort --test-port $boardPort
```

Expect three Unity cases and the same audible tone/silence/pause/recovery sequence
described below. Startup identifies `esp32s3`, I2S1 and two configured cores.
If logs are missed, press RESET once while the test runner is waiting.
Compile only: `pio test -e audio_test_s3 --without-uploading --without-testing`.
For C3, use the existing `audio_test` environment and C3 wiring below.
The base `esp32-s3-n8r8` environment runs the normal blink scaffold, not audio tests.
S3 on-device execution and listening checks remain pending owner validation.

### Hardware gain comparison (pending validation)

Check that the breakout does not already strap GAIN to another connection.
With power disconnected, leave GAIN floating, then power up and run the test above.
Listen for the tone, amplifier shutdown, buffered playback pause and recovery.
Power off, connect GAIN directly to GND, then repeat with the same firmware,
speaker and supply. Do not ground SD_MODE: that shuts the amplifier down.

For MAX98357A I2S operation, floating GAIN selects 9 dB; GAIN directly to GND
selects 12 dB. Expect a modest volume increase (+3 dB) if GAIN was floating,
not twice the perceived loudness. Record any distortion or increased noise.
The test-tone amplitude is unchanged so the comparison is meaningful; this quiet
tone does not establish maximum clean speech volume. Report pin operation and
gain listening results separately from the automated Unity assertions.

## Existing C3 validation and driver contract

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
Validation on 2026-11-05: normal firmware and Unity firmware builds passed with
ESP-IDF 5.5.5. The owner's PlatformIO screenshot confirms upload to COM6 and all
three on-device tests passing: test_output_contract, test_direct_tone, and
test_buffered_playback (3 succeeded; total run 32.484 seconds).
Audible tone, amplifier silence, and pause/recovery observations have not yet
been confirmed by the owner. Network integration and S3 validation remain pending.
