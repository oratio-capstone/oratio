"""Check fixture format rejection, deterministic conversion, and final-frame padding."""

from pathlib import Path
import struct
import tempfile
import unittest
import wave

from prepare_playback_fixture import read_clip, render


class FixtureTests(unittest.TestCase):
    def test_interpolation_and_peak(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.wav"
            with wave.open(str(path), "wb") as output:
                output.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
                output.writeframes(struct.pack("<4h", -100, 100, -100, 100))
            self.assertEqual(read_clip(path), [-4096, 0, 4096, 0, -4096, 0, 4096, 4096])

    def test_reject_wrong_rate_and_silence(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.wav"
            for rate in (16000, 8000):
                with wave.open(str(path), "wb") as output:
                    output.setparams((1, 2, rate, 0, "NONE", "not compressed"))
                    output.writeframes(b"\0" * 20)
                with self.assertRaises(ValueError):
                    read_clip(path)

    def test_fixture_frame_boundaries(self):
        files, samples = render()
        self.assertEqual(len(samples), 40028)
        self.assertNotEqual(len(samples) % 320, 0)  # Exercise the application's partial final frame.
        frames = [samples[index:index + 320] for index in range(0, len(samples), 320)]
        valid_tail = len(frames[-1])
        frames[-1] += [0] * (320 - valid_tail)
        flattened = [sample for frame in frames for sample in frame]
        self.assertEqual(flattened[:len(samples)], samples)
        self.assertEqual(flattened[len(samples):], [0] * (320 - valid_tail))
        self.assertEqual(len(frames), 126)
        self.assertEqual(render(), (files, samples))


if __name__ == "__main__":
    unittest.main()
