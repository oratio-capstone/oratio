"""Compile the real output driver against fault-injecting host I2S test doubles."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    firmware = Path(__file__).resolve().parents[1]
    tests = firmware / "test" / "host_audio_output"
    compiler = shutil.which(os.environ.get("CC", "gcc"))
    if compiler is None:
        raise SystemExit("A host C compiler is required; install GCC or set CC to clang.")

    with tempfile.TemporaryDirectory(prefix="oratio-audio-") as temporary:
        executable = Path(temporary) / ("test_audio_output.exe" if os.name == "nt" else "test_audio_output")
        subprocess.run(
            [
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
                "-I", str(tests / "stubs"), "-I", str(firmware / "lib" / "audio"),
                str(firmware / "lib" / "audio" / "audio_output.c"), str(tests / "test_audio_output.c"),
                "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
    print("PASS: six host suites (arguments/state, packing/deadlines, timeout recovery, faults, init, lifecycle)")


if __name__ == "__main__":
    main()
