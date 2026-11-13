"""Start the whole dashboard setup with one command, for development.

Runs, in one terminal:
  gateway   sw/gateway/main.py                      (UDP 8989 + WebSocket 8000)
  web       python -m http.server 8080 on sw/web    (the dashboard page)
  sim       sw/tools/sim_nodes.py                   (fake nodes)

and opens http://localhost:8080. Ctrl+C stops all three.

The dashboard asks for a password. Unless DASHBOARD_PASSWORD_FILE is set, a throwaway one
(DEV_PASSWORD below) is used, so the real password never needs to be on a laptop.

Usage (with the environment activated, from anywhere):
  python sw/tools/run_dev.py                         # default simulation
  python sw/tools/run_dev.py --nodes 6 --talkers 2   # any sim_nodes.py options
  python sw/tools/run_dev.py --no-sim                # gateway + page only (e.g. with a real ESP)
  python sw/tools/run_dev.py --no-browser            # don't open the browser
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from pathlib import Path

SW = Path(__file__).resolve().parents[1]
WEB_PORT = 8080
DEV_PASSWORD = "oratio-dev"  # development only; the Pi has its own, set with auth.py
DEFAULT_SIM_ARGS = ["--talkers", "1", "--drop", "3:10", "--sos", "5:15", "--drop", "102:25"]
COLORS = {"gateway": "\033[36m", "web": "\033[35m", "sim": "\033[33m"}
RESET = "\033[0m"


def _stream(name: str, proc: subprocess.Popen) -> None:
    color = COLORS.get(name, "") if sys.stdout.isatty() else ""
    reset = RESET if color else ""
    for line in proc.stdout:  # type: ignore[union-attr]
        print(f"{color}[{name:7}]{reset} {line}", end="", flush=True)


def _dev_password_file() -> str:
    sys.path.insert(0, str(SW / "gateway"))
    import auth
    path = Path(tempfile.mkdtemp(prefix="oratio-dev-")) / "dashboard-password"
    auth.write_password_file(path, DEV_PASSWORD)
    return str(path)


def _start(name: str, args: list[str], cwd: Path) -> subprocess.Popen:
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    proc = subprocess.Popen([sys.executable, *args], cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1)
    threading.Thread(target=_stream, args=(name, proc), daemon=True).start()
    return proc


def _stop_on_signal(signum, frame) -> None:
    raise KeyboardInterrupt


def main() -> int:
    # Make Ctrl+C work even if the parent shell started us with SIGINT ignored,
    # and stop cleanly on a normal kill (SIGTERM) too.
    signal.signal(signal.SIGINT, signal.default_int_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _stop_on_signal)
    argv = sys.argv[1:]
    run_sim = "--no-sim" not in argv
    open_browser = "--no-browser" not in argv
    sim_args = [a for a in argv if a not in ("--no-sim", "--no-browser")] or DEFAULT_SIM_ARGS

    password_note = "your DASHBOARD_PASSWORD_FILE"
    if "DASHBOARD_PASSWORD_FILE" not in os.environ:
        os.environ["DASHBOARD_PASSWORD_FILE"] = _dev_password_file()  # inherited by the gateway
        password_note = DEV_PASSWORD

    procs: dict[str, subprocess.Popen] = {
        "gateway": _start("gateway", ["main.py"], SW / "gateway"),
        "web": _start("web", ["-m", "http.server", str(WEB_PORT), "--directory", str(SW / "web")], SW),
    }
    time.sleep(1.0)  # let the gateway bind its ports before nodes start sending
    if run_sim:
        procs["sim"] = _start("sim", [str(SW / "tools" / "sim_nodes.py"), *sim_args], SW)

    url = f"http://localhost:{WEB_PORT}"
    print(f"\nDashboard: {url}   password: {password_note}   (Ctrl+C to stop everything)\n", flush=True)
    if open_browser:
        webbrowser.open(url)

    exit_code = 0
    try:
        while True:
            for name, proc in procs.items():
                if proc.poll() is not None:
                    print(f"\n{name} stopped (exit code {proc.returncode}); stopping the rest.", flush=True)
                    exit_code = proc.returncode or 1
                    raise KeyboardInterrupt
            time.sleep(0.3)
    except KeyboardInterrupt:
        pass
    finally:
        for proc in procs.values():
            if proc.poll() is None:
                proc.terminate()
        for proc in procs.values():
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        print("All stopped.")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
