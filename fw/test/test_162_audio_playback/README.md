# Task 162 speech fixture

Expected words: **one, two, three**, spoken by Jackson in English.

Source: [Free Spoken Digit Dataset](https://github.com/Jakobovski/free-spoken-digit-dataset),
by the FSDD authors and contributors, with speaker identified as Jackson. The original recordings are
`1_jackson_0.wav`, `2_jackson_0.wav`, and `3_jackson_0.wav` from revision
`26eb9aaf76e81b692f806f9140c2d2777410d7a1`.
The [pinned dataset README](https://github.com/Jakobovski/free-spoken-digit-dataset/blob/26eb9aaf76e81b692f806f9140c2d2777410d7a1/README.md)
identifies the dataset license as **Creative Commons Attribution-ShareAlike 4.0 International**.

The source recordings and adapted PCM fixture are redistributed under
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/)
([legal text](https://creativecommons.org/licenses/by-sa/4.0/legalcode.en)).
Preserve this attribution and license when redistributing these assets or adaptations.
No endorsement by the original speaker or dataset authors is implied.
This asset license does not purport to relicense the project's separate playback library.

`sources/manifest.json` records immutable source URLs and SHA-256 hashes. The three original
WAV files are included so regeneration and build checks work offline.

## Adaptations and reproduction

From `fw/`, run:

```powershell
python tools/prepare_playback_fixture.py --check
python tools/test_playback_fixture.py
python tools/prepare_playback_fixture.py
```

Python's standard library is sufficient. Conversion validates uncompressed 8 kHz PCM16 mono
sources, removes each clip's integer mean, normalizes each clip to peak 4096 (about -18 dBFS),
and doubles the sample rate through deterministic linear interpolation. This is a modest test
fixture conversion, not a production resampler; the 8 kHz source's limited bandwidth remains.
It adds 250 ms of silence before the first digit and after each digit, then emits signed
16-bit C sample values at 16 kHz. There is no runtime WAV parser or decoder.

Result: **40,028 samples, 80,056 PCM bytes, 2.50175 seconds**, stored as a `const int16_t` array
in flash. The application submits 126 frames: 125 full frames and 28 valid samples followed
by 292 zeros in the final frame. That padding adds 18.25 ms. The generated `.c` file carries
the adapted audio's CC BY-SA attribution notice.

Only source WAVs, provenance and conversion code are checked in. The manifest also pins the
converted PCM hash to the hardware-tested recording. The last command generates C/header files
under ignored `.pio/generated/speech_fixture`; `--output-dir` selects another build directory.
The firmware CMake configuration generates them automatically under `.pio/build` using its Python
interpreter. Host application tests generate into a temporary directory. Conversion is offline and
uses only the standard library; no download is required to build or test.
The small spoken-digit sequence supports repeatable intelligibility checks; it does not establish
quality for continuous conversation, multiple speakers, network jitter, or packet loss.
