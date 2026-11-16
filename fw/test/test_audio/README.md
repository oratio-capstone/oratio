# Playback tests

Use PlatformIO Core **6.1.19** with the repository's pinned platform. Run from `fw`.

| MAX98357A signal | S3 GPIO |
| --- | --- |
| BCLK | 42 |
| WS / LRC | 2 |
| DIN | 1 |
| SD_MODE | 47 |

Power off before rewiring; use common ground and a compatible amplifier supply.
SD_MODE must not also be tied to a supply. Disconnect serial monitors before upload.

```sh
pio test -e esp32-s3-n8r8 -f test_audio
```

Add `--upload-port COM12 --test-port COM12` using the actual board port.
For compile-only checks, add `--without-uploading --without-testing`.
Expect three Unity cases, a quiet 500 Hz tone, amplifier silence, and buffered
playback with a pause and recovery. Passing assertions alone do not prove audible
output or successful asynchronous writes. Hardware/listening validation is separate.
The S3 uses its native USB Serial/JTAG port.

## Review changes (PR #14)

- `playback.c`: made the receive frame static; replaced the zero-byte receive
  log/suspend branch with `configASSERT(received != 0)`. This asserts the producer
  contract; it does not recover oversized queued messages. Assertions must remain
  enabled. Short-message, sample-length and output-error checks remain.
- `fw/CMakeLists.txt`: removed the audio `EXTRA_COMPONENT_DIRS` entry.
- Deleted `fw/lib/audio/CMakeLists.txt`; restored `fw/src/CMakeLists.txt` to main's
  `idf_component_register(SRCS ${app_sources})`. PlatformIO discovers the library.
- `platformio.ini`: removed `audio_test` and `audio_test_s3`; tests now use the
  board environments above. Kept the S3 board environment and S3 SDK defaults.
- Deleted `sdkconfig.audio_test.defaults`; C3 uses its existing board settings.
- `.gitignore`: retained main's general SDK-config rules and defaults exceptions
  instead of the obsolete test-specific rules when resolving the merge conflict.
- Shortened this README, corrected all S3 wiring, removed the obsolete unrouted
  SD_MODE statement, and updated test commands. Driver code, GPIO definitions,
  test assertions and pipeline contents relative to main are unchanged.
