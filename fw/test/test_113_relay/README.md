# Relay, streaming and multi-hop benchmark

The same test_main.c supports task 113 (finite four-byte relay), task 115
(continuous 640-byte dummy audio), and task 116 (finite audio-rate benchmark).
TEST_BENCHMARK=1 takes precedence over TEST_CONTINUOUS_STREAM. Set it to 0 to
return to task 113/115 selection. The synthetic capture/playback test worker runs on core 1; networking runs on core 0.

## Task 116 setup

Use three boards: gateway <-> root (level 1) <-> middle (level 2) <-> leaf (level 3).
Start a fresh gateway process to discard stale registry entries:

```powershell
python sw/udp_server.py
```

Run that command from the repository root.
Allow inbound UDP 8989 through the laptop firewall. All endpoints use that same
application port; NAT can translate the external source port.

From fw, activate the existing .venv-pio. Close PuTTY on each port before upload.
Use the three existing test environments. TEST_BENCHMARK=1 is already selected
in the shared build flags. No level-flag changes are needed between uploads.

| Board | Environment | Test | Expected mesh level |
| --- | --- | --- | --- |
| Root / producer | root_test | test_113_relay | 1 |
| Middle / relay | node_test | test_109_heartbeat | 2 |
| Leaf / consumer | node_level3_test | test_113_relay | 3 |

node_test has no forced level; node_level3_test adds APP_FORCE_LEVEL3=1.
Replace port placeholders with each board's actual COM port. Flash the middle
and consumer first, then the producer:

```powershell
pio test -e node_test -f test_109_heartbeat --upload-port <MIDDLE_PORT> --without-testing
pio test -e node_level3_test -f test_113_relay --upload-port <LEAF_PORT> --without-testing
pio test -e root_test -f test_113_relay --upload-port <ROOT_PORT> --without-testing
```

The middle only runs networking and heartbeats. Existing per-environment SDK
settings, including hotspot credentials, remain in use.
With all boards powered, run test_102_mesh using node_test on the middle first
if its level is uncertain, confirm level 2, then restore heartbeat firmware.
Check root level 1 and leaf level 3 with the middle's SoftAP MAC as parent.
Do not call a run multi-hop without this evidence. The root waits 60 seconds
after networking starts; reset the producer last if more setup time is needed.
Open PuTTY at 115200 baud after uploads, with a separate terminal for each board.

## Automatic batches

BENCHMARK_RUN_COUNT in test_main.c defaults to 3. Build and flash both endpoints
with the same count, frame count, and sending interval. The existing producer flags
select direction; the middle remains heartbeat-only. No new environments are needed.

Start the consumer first, then the producer. There is one 60-second producer startup
wait per batch, followed by three runs without manual resets. Each run generates
1000 frames, waits five seconds for echoes, and pauses another ten seconds before
the next run. At 20 ms, a normal three-run producer batch takes about 155 seconds
from its startup wait to completion. A missing run can extend the consumer's wait
to its five-minute first-packet timeout. Capture serial output throughout the batch.

Every frame includes a zero-based run index plus a random run ID. Counters and RTT
timestamps are cleared between runs. Old run indexes are ignored by the consumer;
RTT samples must match both the current run ID and index. A future run's first frame
is retained while missing/overdue runs are finalized. Skipped or timed-out runs
count as 1000 missing frames, rather than silently disappearing from the average.
Do not restart either endpoint in the middle of a batch; reset both to start over.

After all runs, each endpoint prints Batch row entries and Batch complete:

```text
Batch row 1: id=... unique=... missing=... loss=...% rate=... frames/s
Batch row 2: ...
Batch row 3: ...
Batch consumer: runs=3 unique=... missing=... loss=...% best/worst=.../...%
Batch quality: duplicates=... out-of-order=... invalid=... mean-run-rate=... frames/s
```

The producer prints matching row numbers/run IDs, buffered/local-drop counts,
followed by aggregate echo results and Batch RTT. Save both endpoints' summaries:
receiver loss and producer RTT remain separate measurements.

Batch loss = total missing / (run count * 1000). Best/worst are the minimum and
maximum per-run loss percentages. mean-run-rate is the arithmetic mean of per-run
arrival rates (zero for no-data runs), excluding startup waits and run gaps.
Batch RTT mean is weighted by the number of valid echo samples, not an unweighted
mean of per-run means. Min/max span all received samples. Zero RTT samples means
there is no latency measurement; printed zero timings must not be interpreted as
zero latency. Echo missing counts only samples successfully buffered for sending;
producer local drops are listed separately.

## Ideal per-run results (actual timings vary):

Ideal per-run results (actual timings vary):

```text
Producer final: generated=1000 buffered=1000 local-drops=0 duration=20.xxx s
RTT: replies=20 missing=0 duplicates=0 invalid=0 min/avg/max=... ms
Benchmark final: ... unique=1000 remaining=0 duplicates=0 out-of-order=0 invalid=0 foreign=0
final-missing=0.00% rate=approximately 50 frames/s payload=approximately 256 kbit/s ...
```

- Final remaining > 0 means frames were not observed by the deadline. Compare
  producer local-drops and network playback-drops/socket-errors before attributing
  missing frames to radio loss. Network Window counters reset every two seconds
  and are not automatically included in the batch loss breakdown.
- out-of-order counts late unique frames, which still count as received.
  duplicates count repeated IDs; invalid indicates size/sequence/pattern failure.
- RTT replies should be 20 when all sampled requests were buffered and echoed.
  There is no fixed latency pass threshold yet: compare measured values under
  the same conditions. Return-path loss is distinct from forward-stream loss.
- stack-free is the minimum unused task stack so far. Reboots, watchdogs,
  overflows, or values close to zero need investigation.
- A no-data run is a failed delivery run, not a zero-loss result.

## Measurements

The default direction follows root_test/node_level3_test: root produces, leaf
consumes. Each run sends 1000 full 640-byte diagnostic frames every 20 ms with
xTaskDelayUntil. Expected payload rate is 256 kbit/s (50 frames/s). These are
not playable PCM; headers, Wi-Fi and echo traffic add overhead.

The consumer tracks IDs 0..999 in static storage. Late arrivals fill missing
slots; duplicates do not inflate unique counts. Frame range, sequence, size,
payload pattern, random run ID and run index are checked.

For each run, the consumer waits up to five minutes for a first valid frame,
then allows the generation period plus five seconds for late packets. Missing
includes local producer drops, transmission failures, network loss, receiver
buffer drops, and packets outside the measurement deadline.

Rate = (unique - 1) / time between first and last unique arrivals. Payload kbit/s
uses that rate times 640 * 8 / 1000. Fewer than two unique arrivals report zero.
This excludes headers and is not radio airtime throughput.

The consumer echoes every 50th frame through its capture buffer and the gateway.
RTT begins at producer enqueue and ends at producer application receipt, including
both endpoint buffers, scheduling/polling (up to about 20 ms), gateway/consumer
processing, and both network directions. It is not one-way audio latency.

To reverse direction, set root_test TEST_IS_PRODUCER=0 and node_test
TEST_IS_PRODUCER=1 (inherited by node_level3_test), then rebuild both endpoints.
Reset the new consumer first and new producer second. The middle remains unchanged.
Afterward restore the producer flags. TEST_BENCHMARK=0 restores task 113/115 mode;
node_test remains the ordinary two-board node environment.

For a slower baseline, set BENCHMARK_INTERVAL_MS=100 and rebuild both endpoints.
Each sending period is then about 100 seconds and target payload rate 51.2 kbit/s.
BENCHMARK_REPORT_MS affects only logging frequency. No jitter buffer or
retransmission is introduced by this benchmark.

## Test payload (task 116 only)

Payload bytes 0..3: frame number, big endian; byte 4: Q request / R reply;
bytes 5..8: random run ID, big endian; bytes 9..12: zero-based run index, big endian;
bytes 13..639: byte index modulo 256. Both endpoints must use this batch-aware
firmware; it is incompatible with the earlier nine-byte diagnostic prefix.
The normal seven-byte Oratio wire header is unchanged. Capture supplies sequence
and length; the network task supplies its own node ID on each outgoing frame.

## Verification status

Compiler checks cover producer/consumer batch modes and existing 113/115 modes.
Batch timing, automatic rollover and aggregate results still need verification on
the three-board hardware. A build alone does not establish latency or packet loss.
