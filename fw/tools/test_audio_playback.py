"""Build the real playback library against threaded FreeRTOS/output test doubles."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    firmware = Path(__file__).resolve().parents[1]
    tests = firmware / "test" / "host_audio_playback"
    compiler = shutil.which(os.environ.get("CC", "gcc"))
    if compiler is None:
        raise SystemExit("Install GCC/Clang with pthreads (MSYS2 UCRT64 GCC on Windows), or set CC.")
    with tempfile.TemporaryDirectory(prefix="oratio-playback-") as temporary:
        executable = Path(temporary) / ("playback.exe" if os.name == "nt" else "playback")
        subprocess.run(
            [
                compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-pthread",
                "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
                "-I", str(tests / "stubs"),
                "-I", str(firmware / "test" / "host_audio_output" / "stubs"),
                "-I", str(firmware / "lib" / "audio"),
                str(firmware / "lib" / "audio" / "audio_playback.c"),
                str(tests / "host_runtime.c"), str(tests / "test_audio_playback.c"),
                "-o", str(executable),
            ], check=True,
        )
        subprocess.run([str(executable)], check=True, timeout=30)
    print("PASS: seven playback suites (ownership, overflow, silence, volume, faults, lifecycle, concurrency)")


if __name__ == "__main__":
    main()
