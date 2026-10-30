# Prerecorded speech playback validation

This standalone Task 162 app exercises the Task 161 playback library through its public API.
It reuses the existing output driver and shared 16 kHz PCM16 mono / 320-sample frame definitions.
No second service tone app is needed; PR #1 retains the independent low-level tone diagnostic.

## Run

Hardware used: ESP32-C3 Super Mini, MAX98357A, 8-ohm 2 W speaker. Working pins are
GPIO4 → BCLK, GPIO5 → WS/LRC, GPIO7 → DIN, with common ground. Retain the amplifier power
and speaker connections used for the successful tone/speech tests.

Open this checkout's `fw` directory in VS Code. Close any existing monitor with Ctrl+C.
PowerShell commands (substitute the current USB port from `device list`):

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
$python = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
& $python tools/test_audio_output.py
& $python tools/test_audio_playback.py
& $python tools/prepare_playback_fixture.py --check
& $python tools/test_playback_fixture.py
& $python tools/test_playback_application.py
& $pio device list
& $pio run -e audio_playback_test
& $pio run -e audio_playback_test -t upload --upload-port COM6
& $pio device monitor -e audio_playback_test --port COM6 -b 115200 --dtr 0 --rts 0
```

Host tests need pthread-capable GCC/Clang. On this Windows setup, add
`C:\msys64\ucrt64\bin` to PATH if GCC is unavailable. Standard `python`/`pio` commands also work
when installed on PATH. The firmware build needs Python's standard library only for fixture generation.

Upload restarts the board. Expect a ten-second countdown, about half a minute of tests, then a
repeated final result every five seconds. Reset repeats the run and disconnects/reconnects USB;
reopen the monitor promptly if it loses output. Use the named environment rather than generic Upload.
Test-specific SDK defaults select native USB as primary console and the 4 MB flash detected on this board.
Existing generated SDK settings override defaults; when migrating an older test checkout, remove only
its ignored `sdkconfig.audio_playback_test` and rebuild to regenerate it from the defaults.

## Sequence and interpretation

| Stage | Expected observation/check |
| --- | --- |
| Normal speech | Understandable one/two/three; all submitted frames accepted without queued drops |
| Pause/recovery | Deliberate 300 ms producer gap, silence counter increases, speech resumes |
| Volume/mute | Speech at 25% sample amplitude, then no speech at 0%; later stages restore 100% |
| Overflow | Deliberate 64-frame burst; choppy fragment expected; drops increase and queue stays <= 4 |
| Stop/restart | Queue clears, submissions while stopped fail, restart emits silence before fresh speech |
| Lifecycle | Five init/start/speech/deinit cycles; state/queue/heap-integrity checks and heap trend logs |

The final software result should be PASS with result/cleanup ESP_OK. Listening is a separate check.
Individual `write_timeouts` are retryable attempts, not failed frames. The test permits bounded retries
and reports the count; non-running state, `write_errors`, or an acceptance deadline still fail.
The original test incorrectly aborted on a retryable startup timeout; a regression test now exercises
that actual application check with the threaded service, including an unrecovered deadline failure.

Normal completion waits for frame acceptance/empty queue, then 100 ms beyond the nominal DMA
capacity before stopping. This is a test allowance, not a drain API. An interrupted stop may cancel
an in-flight frame, so counters are reset before the next acceptance-based wait. Heap readings are
informational after idle-task cleanup; they do not automatically prove absence of leaks.

## Fixture and results

The [fixture README](../test/test_162_audio_playback/README.md) documents the source/license and
conversion. Three checked-in source WAVs are transformed offline into 40,028 signed PCM samples,
with peak 4096. CMake generates the C array/header inside `.pio/build`, stores the array in flash,
and the producer copies 320 samples every 20 ms using one static buffer. Final-frame padding is
292 zeros. No generated C/header, downloaded cache, or firmware binary is committed.

The owner confirmed audible recorded speech and working hardware after the retryable-timeout test
fix. Local driver (six suites), service (seven suites), fixture (three tests), application regression,
and firmware builds passed. A complete successful serial transcript and individual stage checklist
were not supplied; no calibrated quality, loudness, latency, stack, leak or network-load measurements
are claimed. No microphone or network-delivered audio is exercised by this application.

Later integration still requires a packet-to-PCM adapter and an agreed payload byte order. The C3
pin mapping and toolchain must be validated separately on the team's S3 target.
