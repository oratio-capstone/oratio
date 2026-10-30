"""Regression-check Task 162's actual acceptance rule with the threaded playback service."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    firmware = Path(__file__).resolve().parents[1]
    test = firmware / "test"
    compiler = shutil.which(os.environ.get("CC", "gcc"))
    if compiler is None:
        raise SystemExit("Install GCC/Clang with pthreads or set CC.")
    with tempfile.TemporaryDirectory(prefix="oratio-playback-app-") as temporary:
        fixture = Path(temporary) / "speech_fixture"
        subprocess.run([sys.executable, str(firmware / "tools" / "prepare_playback_fixture.py"),
                        "--output-dir", str(fixture)], check=True)
        executable = Path(temporary) / ("application.exe" if os.name == "nt" else "application")
        command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-pthread",
                   "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
                   "-DORATIO_AUDIO_TEST_BCLK_GPIO=4", "-DORATIO_AUDIO_TEST_WS_GPIO=5",
                   "-DORATIO_AUDIO_TEST_DOUT_GPIO=7"]
        for include in [test / "host_audio_playback_app" / "stubs",
                        test / "host_audio_playback" / "stubs", test / "host_audio_playback",
                        test / "host_audio_output" / "stubs", firmware / "lib" / "audio", fixture]:
            command.extend(["-I", str(include)])
        command.extend(str(source) for source in [firmware / "lib" / "audio" / "audio_playback.c",
                       test / "host_audio_playback" / "host_runtime.c",
                       test / "host_audio_playback_app" / "test_application.c",
                       fixture / "speech_fixture.c"])
        subprocess.run(command + ["-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
    print("PASS: application permits retried timeouts and rejects an unrecovered frame timeout")


if __name__ == "__main__":
    main()
