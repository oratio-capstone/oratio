"""Dashboard login: password hashing, sessions, lockout, and the WebSocket handshake."""

import asyncio
import json
import os
import stat

import pytest
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

import auth
from auth import Auth
from ws_server import AUTH_FAILED, WebSocketServer

PASSWORD = "correct horse battery"


@pytest.fixture(scope="module")
def stored():
    return auth.hash_password(PASSWORD)


def test_hash_verifies_only_the_right_password(stored):
    assert PASSWORD not in stored
    assert auth.verify_password(PASSWORD, stored)
    assert not auth.verify_password("wrong", stored)
    assert not auth.verify_password(PASSWORD, "garbage")
    assert auth.hash_password(PASSWORD) != stored  # salted


def test_password_file_is_private_and_needs_a_real_password(tmp_path):
    path = tmp_path / "sub" / "pw"
    with pytest.raises(ValueError):
        auth.write_password_file(path, "short")
    auth.write_password_file(path, PASSWORD)
    assert stat.S_IMODE(os.stat(path).st_mode) == 0o600
    assert Auth.from_file(path).login("a", PASSWORD) is not None


def test_no_password_file_means_locked():
    locked = Auth.from_file("/nonexistent/oratio-pw")
    assert not locked.configured
    assert locked.login("a", "") is None


def test_sessions_lockout_and_logout(stored):
    a = Auth(stored, session_s=100)
    token = a.login("1.2.3.4", PASSWORD, now=0)
    assert token and a.login("1.2.3.4", token=token, now=50) == token
    assert a.login("1.2.3.4", token=token, now=101) is None          # expired
    assert a.login("1.2.3.4", token="made-up", now=0) is None

    for _ in range(auth.MAX_FAILURES):
        assert a.login("6.6.6.6", "guess", now=200) is None
    assert a.login("6.6.6.6", PASSWORD, now=201) is None              # locked out, even with the right one
    assert a.login("1.2.3.4", PASSWORD, now=201) is not None          # other addresses unaffected
    assert a.login("6.6.6.6", PASSWORD, now=200 + auth.LOCKOUT_S + 1) is not None

    token = a.login("1.2.3.4", PASSWORD, now=300)
    a.logout(token)
    assert a.login("1.2.3.4", token=token, now=301) is None


@pytest.fixture
async def server(stored):
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue(), auth=Auth(stored))
    ws.login_failure_delay = 0
    srv = await ws.start("127.0.0.1", 0)
    url = f"ws://127.0.0.1:{srv.sockets[0].getsockname()[1]}/ws"
    yield url
    ws._consumer.cancel()
    ws._talk_task.cancel()
    srv.close()


async def _first(sock):
    return json.loads(await asyncio.wait_for(sock.recv(), 5))


async def _refused(url, login):
    async with connect(url) as sock:
        await sock.send(json.dumps(login))
        assert (await _first(sock))["type"] == "auth_failed"
        with pytest.raises(ConnectionClosed):
            await sock.recv()
        assert sock.close_code == AUTH_FAILED


async def test_nothing_is_sent_before_login(server):
    await _refused(server, {"type": "listen", "node_id": 3})
    await _refused(server, {"type": "login", "password": "nope"})
    await _refused(server, {"type": "login", "token": "made-up"})


async def test_login_then_snapshot_then_token_reconnect_then_logout(server):
    async with connect(server) as sock:
        await sock.send(json.dumps({"type": "login", "password": PASSWORD}))
        ok = await _first(sock)
        assert ok["type"] == "auth_ok"
        assert (await _first(sock))["type"] == "snapshot"
    async with connect(server) as sock:
        await sock.send(json.dumps({"type": "login", "token": ok["token"]}))
        assert (await _first(sock))["type"] == "auth_ok"
        assert (await _first(sock))["type"] == "snapshot"
        await sock.send(json.dumps({"type": "logout"}))
    await _refused(server, {"type": "login", "token": ok["token"]})


async def test_pages_from_other_sites_are_refused(server):
    with pytest.raises(InvalidStatus):
        async with connect(server, origin="http://evil.example"):
            pass
    async with connect(server, origin="http://127.0.0.1:8080") as sock:  # the dashboard itself
        await sock.send(json.dumps({"type": "login", "password": PASSWORD}))
        assert (await _first(sock))["type"] == "auth_ok"
