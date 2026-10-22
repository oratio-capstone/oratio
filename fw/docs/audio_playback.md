# Speaker output bring-up (AB#147 / AB#148)

## Scope and status

This change implements task 147's low-level MAX98357A output wrapper and provides
a standalone tone harness for task 148. It does not implement the task 161
playback queue or task 162 speech test. Physical initialization, audible output,
and the 30-second tone acceptance test require the actual board; they have not
been verified remotely. Do not close either Azure task based only on compilation.

## Implementation and ownership

```text
signed mono PCM samples -> audio_output_write -> duplicate left/right -> IDF TX DMA -> MAX98357A
```

- `lib/audio/audio_common.h` defines the proposed shared application format:
  16 kHz, signed PCM16, mono, 20 ms, 320 samples / 640 bytes. `audio_frame_t`
  contains samples, not a serialized network packet. Coordinate this header
  with capture before merging; no capture implementation was present locally.
- `lib/audio/audio_output.c/.h` owns one standard I2S TX channel on controller 0,
  its lifecycle, and one reusable packing buffer. There are no queues, volume
  controls, per-write heap allocations, or network parsing in this module.
- `lib/audio/CMakeLists.txt` explicitly registers the audio ESP-IDF component.
  The project CMake adds it to `EXTRA_COMPONENT_DIRS`; the application requires
  it. Creating a file under `lib/` alone is not the build integration.
- `src/CMakeLists.txt` selects exactly one `app_main`: the original blink app
  normally, or the direct-output test when `ORATIO_AUDIO_OUTPUT_TEST` is enabled.
- `test/test_148_audio_output/test_main.c` generates a continuous quiet 440 Hz
  tone directly through the output driver, retaining phase between buffers.
- `test/host_audio_output/` and `tools/test_audio_output.py` compile the real
  driver against small fault-injecting IDF doubles. They test software contracts,
  not electrical output or the correctness of Espressif's implementation.

PCM describes amplitude values over time. I2S slots describe how those values
are transported on wires. The output uses standard Philips I2S with two 16-bit
slots, duplicating each mono sample into both slots. Thus amplifier left/right
selection does not discard the only populated channel. Microphone 32-bit slots
do not change the application PCM format or automatically configure this driver.

The amplifier accepts this I2S format without an MCLK or I2C configuration.
See [MAX98357A manufacturer documentation](https://www.analog.com/en/products/max98357a.html).

IDF allocates DMA-capable hardware buffers. The wrapper's ordinary static packing
buffer is copied into them. Four descriptors of 80 stereo sample times each hold
320 sample times total: 20 ms / 1280 hardware bytes, distinct from 640 application
PCM bytes. DMA lets the peripheral clock samples while the CPU does other work.

## API contract

All calls are task-context only. One owner must serialize every operation,
including initialization and cleanup. Do not call the singleton from multiple
tasks, an ISR, or concurrently with the later playback service.

1. `audio_output_init(&config)` validates three distinct output-capable GPIOs,
   allocates TX, and configures stopped hardware. Board-specific availability,
   reserved flash pins, USB pins, and attached devices remain caller concerns.
2. `audio_output_start()` preloads the entire DMA ring with zeroes, then enables
   TX. This adds initial silence of up to approximately one ring duration and
   prevents old audio from being replayed on restart.
3. `audio_output_write(samples, count, &written, timeout_ms)` accepts 1–320 signed
   mono samples. The timeout is 1–1000 ms for the complete I2S wait budget, subject
   to RTOS scheduling/timer resolution, not a hard real-time execution guarantee.
   The limit prevents excessively long waits and tick-conversion overflow.
4. `audio_output_stop()` disables TX immediately; it does not drain pending audio.
5. `audio_output_deinit()` stops if necessary and deletes TX. Calling it again
   after success is harmless. If stop/delete fails, the handle is retained so
   cleanup can be retried rather than silently leaking ownership.

Duplicate init/start/stop calls return `ESP_ERR_INVALID_STATE`. A successful stop
allows start again. Invalid pointers, sample counts, timeouts, and duplicate or
invalid GPIOs return `ESP_ERR_INVALID_ARG`. Initialization errors attempt cleanup.

Write success means **accepted into IDF/DMA**, not physically finished playing.
The input memory may be reused after the call returns, including after a partial
write: IDF copies it and the wrapper retains no pointer to the caller's memory.

`written` reports complete mono samples accepted even when a write times out.
Retry only the remaining input (`samples + written`, `count - written`), with a
fresh wait budget if appropriate. If all samples were accepted despite an error,
there is no remaining input to resubmit. Never resend the complete original frame.
IDF uses a timeout per DMA wait, so the wrapper uses descriptor-sized chunks and
recalculates its remaining overall budget between calls. A chunk can cross two
descriptors after partial output or starvation recovery; each native wait gets
at most half the remaining time. This can return a timeout early, especially at
coarse RTOS tick resolution. It does not multiply a fresh timeout across all
DMA waits. The tone harness supplies a 100 ms budget for each 20 ms frame.

A timeout alone leaves the channel usable. Other write errors, zero-progress
successes, or malformed partial hardware samples fault the wrapper. Stop/deinit
and initialize again before writing; a failed preload/enable also requires deinit.
Do not busy-loop retrying hardware faults. The tone harness logs once and cleans up.

TX auto-clear is enabled: completed DMA buffers become zeroes when the producer
is late, rather than replaying stale samples. This is not a jitter buffer or a
guarantee against scheduler-induced gaps. Task 161 will separately supply silent
frames when its application queue is empty. I2S/DMA writes provide output pacing;
adding a fixed 20 ms delay after every frame would introduce gaps.

## Hardware configuration (provisional)

The user confirmed an ESP32-C3 Super Mini and a MECCANIXITY 8 ohm, 2 W, 57 mm
round speaker. The repository's `esp32-c3-devkitm-1` environment is retained as
the existing C3 build profile; this does not make its DevKitM board layout match
the Super Mini. Confirm the actual board labels/flash capacity, amplifier
breakout details, and supply connection before uploading. No board was flashed.

The standalone test environment proposes the following configurable pins:

| ESP32 signal | MAX98357A breakout | Proposed GPIO |
| --- | --- | --- |
| BCLK output | BCLK / BCK | 4 |
| Word select | LRC / LRCLK / WS | 5 |
| Audio data output | DIN | 7 |
| Common ground | GND | Board ground |
| Supply | VIN | Confirm for the actual breakout and board |

Change the three `ORATIO_AUDIO_TEST_*_GPIO` build flags to match verified wiring
before uploading. The wrapper has no default GPIO mapping and does not control
amplifier gain or shutdown pins; check the breakout's documented strap defaults.
Tone amplitude starts at 512/32768 (about 1.6% of digital full scale). Actual
speaker power also depends on amplifier gain and supply; respect the 2 W rating.

Connect the speaker between the amplifier's speaker output terminals; neither
speaker terminal is a ground connection. Check the actual breakout schematic,
gain/shutdown straps, supply capability, and speaker rating before powering it.

The supplied capture guide reserves GPIO6 for microphone data input. This test
uses GPIO7 for output. GPIO4/5 overlap the proposed microphone clocks: run this as
standalone playback, not two independently initialized drivers driving those
same pins. For combined capture/playback, coordinate controller/channel ownership,
clock wiring, and slot configuration first. C3 has separate TX/RX channels on one
I2S controller; shared-clock full duplex needs compatible settings. S3 integration
and migration are deferred rather than silently adopting the mesh branch's setup.

## Build, upload, and monitor

From `fw/`, with PlatformIO available on PATH:

```powershell
# Normal blink build, including compilation of the registered audio component.
pio run -e esp32-c3-devkitm-1

# Direct-output tone firmware (exactly one app_main; no microphone/network).
pio run -e audio_output_test

# Only after confirming the actual board, pins, amplifier supply, and speaker.
pio run -e audio_output_test -t upload --upload-port COMx
pio device monitor -e audio_output_test --port COMx --baud 115200

# Restore blink firmware when finished.
pio run -e esp32-c3-devkitm-1 -t upload --upload-port COMx
```

Replace `COMx` with the board's actual port (`pio device list`). Close the monitor
before uploading. In VS Code, open `fw/` as the PlatformIO project, select the
desired environment, then use Build, Upload, and Monitor.

The tested SDK configuration enables USB Serial/JTAG secondary console output,
which provides logs on the C3 Super Mini's native USB connection even though
the inherited profile selects UART0 as the primary console. Leave GPIO18/19
available for USB. If no port appears, check the USB data cable and boot/download
mode before changing audio pins or adding Arduino USB build flags.

If `pio` is not on PATH in this Windows installation, substitute:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e audio_output_test
```

These are `pio run` diagnostic firmware builds, **not** `pio test`/Unity tests.
The host doubles are deliberately outside the PlatformIO test runner. Run them
with Python and a host GCC/Clang compiler (`CC` can select another compiler):

```powershell
python tools/test_audio_output.py
```

## Manual hardware checklist (pending)

- Confirm pin mapping, board model, supply, speaker, and breakout strap settings.
- Upload the tone environment. Confirm `AUDIO_OUTPUT` ready/started logs and
  `TEST_148` frame summaries approximately once per second.
- Listen for a continuous quiet 440 Hz tone for at least 30 seconds. Record any
  clicks, interruptions, resets, supply problems, or write failures.
- If a logic analyzer is available, verify WS = 16 kHz and BCLK = 512 kHz
  (16000 x 2 slots x 16 bits), plus matching left/right data.
- Reset or restore blink to end the continuous test. Record board model, wiring,
  supply, speaker, firmware commit, serial errors, and audible result in the PR.

Troubleshoot in order: correct flashed environment and serial port; common ground
and power; breakout shutdown/gain straps; speaker terminals; GPIO wiring; WS/BCLK
and I2S timing. Wrong pitch points toward sample-clock/configuration problems;
resets can indicate supply problems; stale/repeated audio or gaps need DMA and
task-pacing inspection. Do not assume every silent output is a software error.

## Verification record

- Initial unchanged baseline attempt was blocked by sandbox access to PlatformIO's
  user tool directory. Retried with access for the configured toolchain download.
- The platform installer initially failed to install Python prerequisites; an
  explicit check/install of its declared prerequisites succeeded, then the build
  was retried without changing project dependencies.
- Resolved platform: pioarduino 55.03.312; framework headers: ESP-IDF 5.5.5.
  The repository's moving `stable` URL was preserved; no framework upgrade or pin
  was introduced in this change. This differs from the mesh feature branch's 5.4.2.
- Builds report a flash-size configuration warning: the inherited board profile
  advertises 4 MB while SDK configuration selects 2 MB. This was not a physical
  flash measurement (no board was connected). The original main configuration
  also selects 2 MB. Confirm the Super Mini's flash capacity before choosing a
  different flash/partition configuration; no such change is bundled here.
- Host compiler: MSYS2 GCC 15.2.0, C11, warnings including conversions/shadowing
  treated as errors. All six suites passed: argument/state validation; stereo
  packing/partial writes/deadline; timeout/resume; write faults; initialization
  and cleanup failures; stop/restart/retry cleanup.
- PASS: original `main` firmware snapshot at `4437012`, built with
  `pio run -e esp32-c3-devkitm-1` (529.69 seconds). The temporary snapshot needed
  valid main-commit Git metadata and a fresh CMake configuration; no original
  firmware source changes were required.
- PASS: `pio run -e audio_output_test` (357.61 seconds). The real ESP-IDF compiler
  compiled `audio_output.c` and the selected tone entry point, linked the image,
  and generated bootloader, application, partition, and factory binaries.
  Reported static RAM: 11800 bytes; program size: 204684 bytes. Dynamic IDF driver
  and DMA allocations are additional runtime memory, not included in that RAM number.
- PASS: normal blink build with the registered audio component,
  `pio run -e esp32-c3-devkitm-1` (543.33 seconds). Static RAM: 9816 bytes;
  program size: 169836 bytes, matching the baseline's reported sizes. The
  component is compiled while the unchanged blink entry point remains selected.
- An early overlapping project-environment build invalidated intermediate files;
  those artifacts were discarded and the project builds were rerun sequentially.
  Failed setup/intermediate runs are not counted as successful verification.
- No physical initialization, flash, waveform capture, or audible test performed.

## GitHub and Azure workflow

Branch: `mohamed/feature/147-audio-output-driver`, based on GitHub `main` at
`44370122c6bc58e9e7e18b4c0351bd78e81e7ac1`. This follows the wiki's
`<username>/<type>/<work-item-id>-<short-description>` convention.

Reference `AB#147` and `AB#148` in the commit and PR; task 148's tone harness is
included to make task 147 reviewable. Do not claim hardware acceptance or close
the tasks before it is demonstrated. GitHub is the development source; Azure
Repos is a one-way mirror. Do not edit the mirror or merge directly into main.

## References

- [ESP-IDF 5.5.5 C3 I2S API](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/peripherals/i2s.html)
- [MAX98357A manufacturer documentation](https://www.analog.com/en/products/max98357a.html)
- `STYLE_GUIDE.md`: types, errors, static ownership, logging, formatting, and memory rules.
