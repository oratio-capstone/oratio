"""Compile the real audio capture library against host I2S/FreeRTOS test doubles and run its contract tests."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    firmware = Path(__file__).resolve().parents[1]
    tests = firmware / "test" / "host_audio_capture"
    compiler = shutil.which(os.environ.get("CC", "gcc"))
    if compiler is None:
        raise SystemExit("A host C compiler is required; install GCC or set CC to clang.")

    with tempfile.TemporaryDirectory(prefix="oratio-capture-") as temporary:
        executable = Path(temporary) / ("test_audio_capture.exe" if os.name == "nt" else "test_audio_capture")
        subprocess.run(
            [
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
                "-I", str(tests / "stubs"), "-I", str(firmware / "lib" / "audio"),
                str(tests / "test_audio_capture.c"),
                "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
    print("PASS: audio capture host contract tests")


if __name__ == "__main__":
    main()
