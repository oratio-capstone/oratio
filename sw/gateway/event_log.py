"""Event log: every alert and notable event, saved on the Pi so history survives restarts.

Stored in SQLite (part of Python, nothing to install). All database work runs on one
background thread, in order, so a slow SD card never stalls the event loop that
carries audio. Writes return immediately; reads return a future.

    log = EventLog("~/.local/share/oratio/events.db")
    event = log.add("sos", node_id=4, message="SOS from Node 4", data={"acked": False})
    log.update(event["id"], {"acked": True, "acked_ts": ...})
    rows = await asyncio.wrap_future(log.query(node_id=4, kinds=["sos"]))

An event is a dict: {"id", "ts", "kind", "node_id", "message", **data}. The id is the
alert id the dashboards use, and keeps counting up across restarts.
"""

from __future__ import annotations

import concurrent.futures
import json
import logging
import sqlite3
import time
from pathlib import Path
from typing import Any

log = logging.getLogger("events")

# Kinds that also appear in the Live tab's alert list. The rest are log-only.
ALERT_KINDS = ("sos", "node_dropped", "node_rejoined", "relay_dropped")
MAX_QUERY_ROWS = 50_000

_SCHEMA = """
CREATE TABLE IF NOT EXISTS events (
    id      INTEGER PRIMARY KEY,
    ts      REAL    NOT NULL,
    kind    TEXT    NOT NULL,
    node_id INTEGER,
    message TEXT    NOT NULL,
    data    TEXT    NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS events_ts   ON events (ts);
CREATE INDEX IF NOT EXISTS events_node ON events (node_id, ts);
"""


def _row(row: sqlite3.Row) -> dict[str, Any]:
    event = {"id": row["id"], "ts": row["ts"], "kind": row["kind"], "node_id": row["node_id"],
             "message": row["message"]}
    try:
        event.update(json.loads(row["data"]))
    except ValueError:
        pass
    return event


class EventLog:
    def __init__(self, path: str | Path | None, *, retention_days: float = 90) -> None:
        """path=None keeps the log in memory only (tests, or no writable disk)."""
        self.path = Path(path).expanduser() if path else None
        self._pool = concurrent.futures.ThreadPoolExecutor(max_workers=1, thread_name_prefix="event-log")
        self._db: sqlite3.Connection | None = None
        self._next_id = 1
        self._pool.submit(self._open, retention_days).result()  # once, at startup

    # ---- runs on the log thread --------------------------------------------------

    def _open(self, retention_days: float) -> None:
        target = ":memory:"
        if self.path:
            try:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                target = str(self.path)
            except OSError as exc:
                log.warning("can't create %s (%s); keeping the event log in memory", self.path.parent, exc)
        try:
            self._db = sqlite3.connect(target, check_same_thread=False)
        except sqlite3.Error as exc:
            log.warning("can't open %s (%s); keeping the event log in memory", target, exc)
            self._db = sqlite3.connect(":memory:", check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA journal_mode=WAL")    # fewer, smaller writes: kinder to SD cards
        self._db.execute("PRAGMA synchronous=NORMAL")
        self._db.executescript(_SCHEMA)
        cutoff = time.time() - retention_days * 86400
        removed = self._db.execute("DELETE FROM events WHERE ts < ?", (cutoff,)).rowcount
        self._db.commit()
        if removed:
            log.info("removed %d events older than %g days", removed, retention_days)
        self._next_id = (self._db.execute("SELECT MAX(id) FROM events").fetchone()[0] or 0) + 1
        log.info("event log: %s", target)

    def _insert(self, event: dict[str, Any]) -> None:
        core = {"id", "ts", "kind", "node_id", "message"}
        data = {k: v for k, v in event.items() if k not in core}
        try:
            self._db.execute("INSERT INTO events (id, ts, kind, node_id, message, data) VALUES (?,?,?,?,?,?)",
                             (event["id"], event["ts"], event["kind"], event["node_id"], event["message"],
                              json.dumps(data)))
            self._db.commit()
        except sqlite3.Error as exc:
            log.warning("could not save event %s: %s", event.get("id"), exc)

    def _update(self, event_id: int, changes: dict[str, Any]) -> None:
        try:
            row = self._db.execute("SELECT data FROM events WHERE id = ?", (event_id,)).fetchone()
            if row is None:
                return
            data = json.loads(row["data"])
            data.update(changes)
            self._db.execute("UPDATE events SET data = ? WHERE id = ?", (json.dumps(data), event_id))
            self._db.commit()
        except (sqlite3.Error, ValueError) as exc:
            log.warning("could not update event %s: %s", event_id, exc)

    def _where(self, node_id: int | None, kinds: list[str] | None, since: float | None,
               until: float | None, before_id: int | None) -> tuple[str, list[Any]]:
        clauses, args = [], []
        if node_id is not None:
            clauses.append("node_id = ?")
            args.append(node_id)
        if kinds:
            clauses.append(f"kind IN ({','.join('?' * len(kinds))})")
            args.extend(kinds)
        if since is not None:
            clauses.append("ts >= ?")
            args.append(since)
        if until is not None:
            clauses.append("ts < ?")
            args.append(until)
        if before_id is not None:
            clauses.append("id < ?")
            args.append(before_id)
        return (" WHERE " + " AND ".join(clauses)) if clauses else "", args

    def _query(self, node_id, kinds, since, until, before_id, limit) -> list[dict[str, Any]]:
        where, args = self._where(node_id, kinds, since, until, before_id)
        limit = max(1, min(int(limit), MAX_QUERY_ROWS))
        rows = self._db.execute(f"SELECT * FROM events{where} ORDER BY id DESC LIMIT ?", (*args, limit))
        return [_row(r) for r in rows]

    def _stats(self, node_id, since, until) -> dict[str, Any]:
        where, args = self._where(node_id, None, since, until, None)
        counts = {r["kind"]: r["n"] for r in
                  self._db.execute(f"SELECT kind, COUNT(*) AS n FROM events{where} GROUP BY kind", args)}
        ack_times, open_sos = [], 0
        sos_where = where + (" AND " if where else " WHERE ") + "kind = 'sos'"
        for r in self._db.execute(f"SELECT * FROM events{sos_where}", args):
            event = _row(r)
            if event.get("acked") and event.get("acked_ts"):
                ack_times.append(event["acked_ts"] - event["ts"])
            else:
                open_sos += 1
        ack_times.sort()
        return {
            "counts": counts,
            "sos": counts.get("sos", 0),
            "sos_open": open_sos,
            "ack_avg_s": sum(ack_times) / len(ack_times) if ack_times else None,
            "ack_median_s": ack_times[len(ack_times) // 2] if ack_times else None,
            "ack_max_s": ack_times[-1] if ack_times else None,
        }

    # ---- called from the event loop ----------------------------------------------

    def add(self, kind: str, *, node_id: int | None, message: str, ts: float | None = None,
            **data: Any) -> dict[str, Any]:
        """Record an event and return it (with its id) straight away; it is saved in the background."""
        event = {"id": self._next_id, "ts": ts or time.time(), "kind": kind, "node_id": node_id,
                 "message": message, **data}
        self._next_id += 1
        self._pool.submit(self._insert, dict(event))
        return event

    def update(self, event_id: int, changes: dict[str, Any]) -> None:
        self._pool.submit(self._update, event_id, dict(changes))

    def query(self, *, node_id: int | None = None, kinds: list[str] | None = None,
              since: float | None = None, until: float | None = None, before_id: int | None = None,
              limit: int = 200) -> concurrent.futures.Future:
        """Newest first. Resolves to a list of event dicts."""
        return self._pool.submit(self._query, node_id, kinds, since, until, before_id, limit)

    def stats(self, *, node_id: int | None = None, since: float | None = None,
              until: float | None = None) -> concurrent.futures.Future:
        """Counts per kind and SOS acknowledgement times for the report."""
        return self._pool.submit(self._stats, node_id, since, until)

    def recent(self, kinds: tuple[str, ...], limit: int, since: float | None = None) -> list[dict[str, Any]]:
        """Blocking read for startup: the newest events of these kinds, oldest first."""
        return list(reversed(self.query(kinds=list(kinds), since=since, limit=limit).result()))

    def close(self) -> None:
        def _close() -> None:
            if self._db:
                self._db.close()
        self._pool.submit(_close)
        self._pool.shutdown(wait=True)
