"""Dashboard login: one shared password, kept on the Pi as a salted scrypt hash.

The password itself is never stored or committed. Set or change it on the Pi with:

    python auth.py set            # asks twice, writes DASHBOARD_PASSWORD_FILE (mode 600)

A browser logs in once and gets a session token, which it sends instead of the
password when it reconnects. Tokens live in memory, so a gateway restart logs
everyone out. Repeated wrong passwords from one address are slowed down.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import getpass
import hashlib
import hmac
import os
import secrets
import sys
import time
from pathlib import Path

SCRYPT_N, SCRYPT_R, SCRYPT_P = 2 ** 14, 8, 1
MIN_PASSWORD_LENGTH = 8
SESSION_S = 12 * 3600          # a login lasts one long shift
MAX_FAILURES = 5               # wrong passwords from one address before it is locked out
LOCKOUT_S = 60.0


def _b64(raw: bytes) -> str:
    return base64.b64encode(raw).decode("ascii")


def hash_password(password: str) -> str:
    """'scrypt$n$r$p$salt$hash', safe to store: the password can't be read back from it."""
    salt = secrets.token_bytes(16)
    digest = hashlib.scrypt(password.encode(), salt=salt, n=SCRYPT_N, r=SCRYPT_R, p=SCRYPT_P)
    return f"scrypt${SCRYPT_N}${SCRYPT_R}${SCRYPT_P}${_b64(salt)}${_b64(digest)}"


def verify_password(password: str, stored: str) -> bool:
    try:
        scheme, n, r, p, salt, digest = stored.strip().split("$")
        if scheme != "scrypt":
            return False
        expected = base64.b64decode(digest)
        actual = hashlib.scrypt(password.encode(), salt=base64.b64decode(salt),
                                n=int(n), r=int(r), p=int(p), dklen=len(expected))
    except (ValueError, TypeError):
        return False
    return hmac.compare_digest(actual, expected)


def write_password_file(path: str | Path, password: str) -> None:
    if len(password) < MIN_PASSWORD_LENGTH:
        raise ValueError(f"the password needs at least {MIN_PASSWORD_LENGTH} characters")
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)  # only the gateway's user can read it
    with os.fdopen(fd, "w") as f:
        f.write(hash_password(password) + "\n")
    os.replace(tmp, path)


class Auth:
    """Checks passwords and session tokens for the WebSocket server."""

    def __init__(self, password_hash: str, session_s: float = SESSION_S) -> None:
        self.password_hash = password_hash.strip()
        self.session_s = session_s
        self._sessions: dict[str, float] = {}             # token -> expiry
        self._failures: dict[str, list[float]] = {}       # address -> times of recent wrong passwords

    @classmethod
    def from_file(cls, path: str | Path) -> "Auth":
        """No file = no password set: every login is refused (the dashboard stays locked)."""
        try:
            return cls(Path(path).read_text())
        except FileNotFoundError:
            return cls("")

    @property
    def configured(self) -> bool:
        return bool(self.password_hash)

    def locked_out(self, address: str, now: float | None = None) -> bool:
        now = time.time() if now is None else now
        recent = [t for t in self._failures.get(address, []) if now - t < LOCKOUT_S]
        self._failures[address] = recent
        return len(recent) >= MAX_FAILURES

    def login(self, address: str, password: str | None = None, token: str | None = None,
              now: float | None = None) -> str | None:
        """A session token if the password or token is good, else None."""
        now = time.time() if now is None else now
        self._sessions = {t: exp for t, exp in self._sessions.items() if exp > now}
        if token is not None and self._sessions.get(token, 0) > now:
            return token
        if password is None or self.locked_out(address, now):
            return None
        if not verify_password(password, self.password_hash):
            self._failures.setdefault(address, []).append(now)
            return None
        self._failures.pop(address, None)
        new_token = secrets.token_urlsafe(32)
        self._sessions[new_token] = now + self.session_s
        return new_token

    async def login_async(self, address: str, password: str | None = None, token: str | None = None,
                          now: float | None = None) -> str | None:
        """Like login(), but the slow password hash runs in a thread: the gateway's UDP routing
        shares this event loop and must never wait on a login."""
        now = time.time() if now is None else now
        if token is not None or password is None or self.locked_out(address, now):
            return self.login(address, None, token, now)
        ok = await asyncio.to_thread(verify_password, password, self.password_hash)
        if not ok:
            self._failures.setdefault(address, []).append(now)
            return None
        self._failures.pop(address, None)
        new_token = secrets.token_urlsafe(32)
        self._sessions[new_token] = now + self.session_s
        return new_token

    def logout(self, token: str) -> None:
        self._sessions.pop(token, None)


def main(argv: list[str] | None = None) -> int:
    import config

    parser = argparse.ArgumentParser(description="Manage the dashboard password.")
    parser.add_argument("command", choices=["set"], help="set or change the password")
    parser.add_argument("--file", default=config.DASHBOARD_PASSWORD_FILE,
                        help=f"where to save the hash (default {config.DASHBOARD_PASSWORD_FILE})")
    args = parser.parse_args(argv)

    password = os.environ.get("DASHBOARD_PASSWORD")  # for scripts; otherwise ask
    if password is None:
        password = getpass.getpass("New dashboard password: ")
        if getpass.getpass("Again: ") != password:
            print("The passwords don't match.", file=sys.stderr)
            return 1
    try:
        write_password_file(args.file, password)
    except ValueError as err:
        print(err, file=sys.stderr)
        return 1
    print(f"Saved. Restart the gateway to use it: {args.file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
