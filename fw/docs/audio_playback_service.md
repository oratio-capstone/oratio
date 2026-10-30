# Audio playback library

One reusable service accepts 16 kHz PCM16 mono: 320 samples / 640 bytes per 20 ms frame,
as defined by `audio_common.h`. It reuses the existing `audio_output` driver unchanged.

## API and ownership

| API | Contract |
| --- | --- |
| `audio_playback_init(config)` | Copy distinct output GPIOs; allocate task/queue/semaphore; enter STOPPED |
| `audio_playback_start()` | STOPPED only; worker initializes output and enters RUNNING |
| `audio_playback_submit(samples, count)` | RUNNING only; copy exactly 320 samples; caller may reuse memory on return |
| `audio_playback_set_volume(percent)` | Initialized service; 0–100% sample amplitude; 0 mutes subsequent frames |
| `audio_playback_get_stats(stats)` | Synchronized state, last error, counters, occupancy/high-water, and volume |
| `audio_playback_stop()` | Reject submissions, cancel output, discard queue; already stopped succeeds; not a drain |
| `audio_playback_deinit()` | Stop successfully, then release resources; already uninitialized succeeds |

One supervisor serializes lifecycle calls. Producers may submit/read stats/set volume concurrently.
Calls are task-context only. The worker exclusively owns the driver/I2S channel until successful
deinit. Source samples must remain valid and unchanged during submit. Success means accepted,
not physically played. No allocations occur in steady-state service processing.

The private four-frame queue stores 80 ms / 2560 bytes. Overflow drops the oldest queued frame;
retained frames remain FIFO. Empty queues emit zeros; driver DMA auto-clear remains enabled.
I2S writes pace output. Short critical sections serialize queue/state access; submission never
waits for queue space. Full-queue replacement copies up to two 640-byte frames under the lock.

The worker has a provisional 4096-byte stack, idle priority + 2, no core pinning, and two static
640-byte buffers. Queue/task/semaphore structures and driver/DMA allocations are additional.
Working-frame and nominal DMA capacity each add up to 20 ms beyond the queue; these are
capacities, not measured latency. Profile stack use and critical-section cost under network load.

## Failures and diagnostics

Partial writes retry only the unaccepted suffix, with a 100 ms frame budget and at most 16 attempts.
Zero-progress timeouts yield. Failed frame writes stop the worker in FAULTED; successful stop allows
restart. Start/stop waits are bounded to 1000 ms subject to scheduling. Timeout or cleanup failure
retains resources for later stop/deinit retry. Only the worker performs driver cleanup.

Counters reset on init, persist across stop/start, and wrap as uint32 values. Output/silence frames
count driver acceptance, not physical completion. `write_timeouts` counts retryable attempts;
`write_errors` counts failed frame writes outside explicit cancellation. Queued overflow/cleanup
drops exclude the in-flight cancelled frame. Idle underflows are expected. Aggregate logging is
limited to once per two seconds, plus state/failure events.

## Integration

```c
audio_frame_t frame;
/* Fill every sample with validated PCM. */
esp_err_t result = audio_playback_submit(frame.samples, AUDIO_SAMPLES_PER_FRAME);
```

The current mesh interface queues `oratio_packet_t`, not `audio_frame_t`. An adapter must validate
audio type/640-byte length, decode the agreed PCM byte order, and call submit. Never pass this
private queue to the packet interface or cast an unaligned payload to `int16_t *`. Byte order,
speaker selection and packet ordering/loss policy still need team agreement. The shared audio
definitions remain unchanged for capture reuse.

The mesh branch targets S3 with a different SDK baseline; validate toolchain, GPIOs, and I2S
ownership separately. No PSRAM is assumed on C3. Networking, capture, mixing and decoding are out of scope.

## Verification

From `fw/`, with Python and pthread-capable GCC/Clang on PATH (MSYS2 UCRT64 GCC on Windows):

```text
python tools/test_audio_output.py
python tools/test_audio_playback.py
pio run -e esp32-c3-devkitm-1
```

The six driver suites and seven service suites passed locally. The threaded service tests exercise
copying/FIFO, overflow, silence, volume, allocation rollback, partial writes, failures, cleanup retry,
stop/restart and concurrent producers. They do not emulate hardware DMA, interrupts or stack use.

The owner reports audible speech and working hardware through Task 162 on an ESP32-C3 Super Mini,
MAX98357A and 8-ohm 2 W speaker, using GPIO4/5/7. No calibrated quality/latency or memory-leak
measurements are claimed. Task 162 owns hardware validation; a second service tone app is redundant.
PR #1's direct-output tone app remains useful for isolating the low-level driver.
