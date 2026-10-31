"""Record the audio_capture_record test output to a WAV file and report measurable audio metrics.

Flash the recording build first:
    pio run -e audio_capture_record -t upload

Then either read the board directly (needs `pip install pyserial`, close any serial monitor first):
    python tools/record_capture_wav.py --port /dev/cu.usbmodem101 --out capture.wav

or save a monitor log and convert it afterwards:
    pio device monitor -e audio_capture_record > capture_log.txt     (Ctrl+C after "PCM_END")
    python tools/record_capture_wav.py --log capture_log.txt --out capture.wav
"""

import argparse
import base64
import math
import re
import sys
import time
import wave

SAMPLE_RATE_HZ = 16000
FRAME_BYTES = 640
PCM_LINE = re.compile(r"PCM (\d+) ([A-Za-z0-9+/=]+)")


def parse_lines(lines):
    """Collect frames between PCM_BEGIN and PCM_END. Returns (frames, saw_begin, saw_end)."""
    frames = []
    saw_begin = saw_end = False
    for line in lines:
        if "PCM_BEGIN" in line:
            frames.clear()
            saw_begin, saw_end = True, False
            continue
        if "PCM_END" in line:
            saw_end = True
            break
        if not saw_begin:
            continue
        match = PCM_LINE.search(line)
        if match:
            frames.append((int(match.group(1)), base64.b64decode(match.group(2))))
    return frames, saw_begin, saw_end


def serial_lines(port, baud, timeout_s):
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial is required for --port: pip install pyserial (or use --log)")

    deadline = time.monotonic() + timeout_s
    with serial.Serial(port, baud, timeout=1) as connection:
        print(f"Listening on {port}; press RESET on the board if nothing happens...")
        while time.monotonic() < deadline:
            raw = connection.readline()
            if raw:
                yield raw.decode("utf-8", errors="replace")
    print("Timed out waiting for PCM_END")


def analyse(frames):
    samples = []
    missing = out_of_order = short = 0
    expected = None
    for sequence, payload in frames:
        if expected is not None and sequence != expected:
            distance = (sequence - expected) & 0xFFFF
            if distance < 0x8000:
                missing += distance
            else:
                out_of_order += 1
        expected = (sequence + 1) & 0xFFFF
        if len(payload) != FRAME_BYTES:
            short += 1
        for index in range(0, len(payload) - 1, 2):
            samples.append(int.from_bytes(payload[index:index + 2], "little", signed=True))
    return samples, missing, out_of_order, short


def dbfs(value):
    return 20 * math.log10(value / 32768) if value > 0 else float("-inf")


def report(frames, samples, missing, out_of_order, short):
    duration = len(samples) / SAMPLE_RATE_HZ
    peak = max((abs(s) for s in samples), default=0)
    rms = math.sqrt(sum(s * s for s in samples) / len(samples)) if samples else 0.0
    dc = sum(samples) / len(samples) if samples else 0.0
    clipped = sum(1 for s in samples if s in (32767, -32768))

    # Quietest and loudest 100 ms windows: noise floor vs speech level.
    window = SAMPLE_RATE_HZ // 10
    levels = []
    for start in range(0, len(samples) - window + 1, window):
        chunk = samples[start:start + window]
        levels.append(math.sqrt(sum(s * s for s in chunk) / window))

    print(f"Frames:            {len(frames)}  ({duration:.2f} s of audio)")
    print(f"Missing frames:    {missing}   out of order: {out_of_order}   short frames: {short}")
    print(f"Peak level:        {peak}  ({dbfs(peak):.1f} dBFS)")
    print(f"RMS level:         {rms:.0f}  ({dbfs(rms):.1f} dBFS)")
    print(f"DC offset:         {dc:.0f}")
    print(f"Clipped samples:   {clipped}  ({100 * clipped / max(len(samples), 1):.2f} %)")
    if levels:
        floor, loudest = min(levels), max(levels)
        print(f"Noise floor:       {dbfs(floor):.1f} dBFS (quietest 100 ms)")
        print(f"Loudest 100 ms:    {dbfs(loudest):.1f} dBFS")
        if floor > 0:
            print(f"Dynamic range:     {dbfs(loudest) - dbfs(floor):.1f} dB (speech vs room)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial port, e.g. /dev/cu.usbmodem101")
    source.add_argument("--log", help="saved monitor output containing PCM lines")
    parser.add_argument("--out", default="capture.wav", help="WAV file to write")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=60, help="seconds to wait when reading --port")
    args = parser.parse_args()

    if args.log:
        with open(args.log, encoding="utf-8", errors="replace") as log:
            frames, saw_begin, saw_end = parse_lines(log)
    else:
        frames, saw_begin, saw_end = parse_lines(serial_lines(args.port, args.baud, args.timeout))

    if not saw_begin or not frames:
        sys.exit("No PCM frames found. Is the audio_capture_record build flashed?")
    if not saw_end:
        print("Warning: PCM_END not seen; recording may be incomplete")

    samples, missing, out_of_order, short = analyse(frames)
    with wave.open(args.out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE_HZ)
        wav.writeframes(b"".join(payload for _, payload in frames))  # payload is already 16-bit little-endian

    print(f"Wrote {args.out}")
    report(frames, samples, missing, out_of_order, short)
    sys.exit(1 if missing or out_of_order or short else 0)


if __name__ == "__main__":
    main()
