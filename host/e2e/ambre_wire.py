#!/usr/bin/env python3
"""Python client for the frozen Ambre wire protocol (target/edr_ipc.h).

Why this module exists (honesty note): the frozen CLI ``edrctl --journal
<sock> status --json`` is built today WITHOUT ``ambre_port_conn_request()``
in the portability layer, so the real binary answers "status unavailable"
(the gap is documented at the top of target/ctl/edr_ctl.c and cannot be
fixed from host/ because target/** belongs to other agents). This module
speaks EXACTLY the same frozen protocol — AF_UNIX SOCK_SEQPACKET, the
16-byte EDR1 header, the request/response bodies of target/edr_ipc.h and
target/ctl/edr_ipc_wire.h — so the e2e bench can probe a live journal.

``run_edrctl()`` still shells out to the real binary when it is present,
and the bench records which probe actually answered, so a report never
claims more than what ran.

JSON keys mirror edrctl's own formatters (target/ctl/edr_ctl.c) so both
probes are interchangeable. English only, Python 3 standard library only.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import subprocess
import time

# -- frozen constants (target/edr_ipc.h, target/port/ambre_port.h) ----------

IPC_MAGIC = 0x45445231          # "EDR1"
IPC_VERSION = 1
FRAME_MAX = 4096
BLOCK_SIZE = 512

MSG_STATUS = 0x30
MSG_LIST = 0x31
MSG_EXPORT = 0x32
MSG_VERIFY = 0x33
MSG_ACK = 0x80
MSG_NAK = 0x81
MSG_STATUS_REPLY = 0xB0
MSG_LIST_REPLY = 0xB1
MSG_EXPORT_REPLY = 0xB2         # target/ctl/edr_ipc_wire.h
MSG_VERIFY_REPLY = 0xB3

PEER_SERVICE = 4                # ambre_peer_type_t, announced in `reserved`
FLAG_LOG_READ = 0x01            # CTL_REQ_FLAG_LOG_READ (EXP-06)

HDR = struct.Struct("<IBBBBII")             # 16 bytes, packed, little-endian
STATUS_BODY = struct.Struct("<BBBBIIIIIIQQQQQ")   # 68 bytes
EVENT_LINE = struct.Struct("<IBBBBQQII")          # 32 bytes
VERIFY_BODY = struct.Struct("<IIIBB")             # 14 bytes
NAK_BODY = struct.Struct("<iI")                   # 8 bytes
EXPORT_HDR = struct.Struct("<IIII")               # 16 bytes

STATES = {0: "empty", 1: "open", 2: "sealed"}
REASONS = {0: "none", 1: "dv_long", 2: "dv_lat", 3: "restraint",
           4: "dv25_lock", 5: "vru", 6: "emergency_brake", 7: "aeb",
           8: "service"}
BACKENDS = {1: "posix", 2: "qnx"}
HEALTH = {0: "ok", 1: "storage_fault", 2: "memory_full"}


class WireError(Exception):
    """Transport or protocol failure (timeout, NAK, malformed reply)."""


def build_frame(msg_type: int, body: bytes = b"", flags: int = 0,
                peer: int = PEER_SERVICE) -> bytes:
    """One request datagram: 16-byte EDR1 header + body."""
    hdr = HDR.pack(IPC_MAGIC, IPC_VERSION, msg_type, flags, peer,
                   len(body), 0)
    return hdr + body


def parse_header(data: bytes):
    """(msg_type, body_len) of a received datagram, or raise WireError."""
    if len(data) < HDR.size:
        raise WireError("short datagram: %d bytes" % len(data))
    magic, version, msg_type, _flags, _peer, body_len, _pad = \
        HDR.unpack(data[:HDR.size])
    if magic != IPC_MAGIC or version != IPC_VERSION:
        raise WireError("bad magic/version: %08x v%d" % (magic, version))
    if len(data) < HDR.size + body_len:
        raise WireError("truncated body: %d < %d" % (len(data), body_len))
    return msg_type, body_len


class JournalWire:
    """Service-tool client of one edr-journal (peer type SERVICE)."""

    def __init__(self, sock_path: str, timeout: float = 2.0):
        self.path = sock_path
        self.timeout = timeout
        self.sock = None

    def connect(self) -> "JournalWire":
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        sock.settimeout(self.timeout)
        sock.connect(self.path)
        self.sock = sock
        return self

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def __enter__(self) -> "JournalWire":
        return self.connect()

    def __exit__(self, *exc) -> None:
        self.close()

    def request(self, msg_type: int, body: bytes = b"",
                flags: int = 0) -> bytes:
        """Send one request, return the reply datagram body-checked."""
        if self.sock is None:
            raise WireError("not connected")
        self.sock.sendall(build_frame(msg_type, body, flags))
        try:
            data = self.sock.recv(FRAME_MAX)
        except socket.timeout as exc:
            raise WireError("reply timeout (%.1f s)" % self.timeout) from exc
        got_type, _ = parse_header(data)
        if got_type == MSG_NAK:
            code, event_id = NAK_BODY.unpack(data[HDR.size:HDR.size + 8])
            raise WireError("journal refused the request (NAK code %d, "
                            "event %d)" % (code, event_id))
        return data

    # -- the four service commands, same JSON shape as edrctl ---------------

    def status(self) -> dict:
        data = self.request(MSG_STATUS)
        got_type, _ = parse_header(data)
        if got_type != MSG_STATUS_REPLY:
            raise WireError("unexpected reply type 0x%02x" % got_type)
        fields = STATUS_BODY.unpack(data[HDR.size:HDR.size + STATUS_BODY.size])
        keys = ("mode", "health", "backend", "supervised", "current_seg",
                "current_block", "open_events", "acquired_events",
                "sealed_events", "refused_no_space", "bytes_written",
                "flush_count", "uptime_ms", "last_freeze_us",
                "worst_freeze_us")
        out = dict(zip(keys, fields))
        out["mode"] = "continuous" if out["mode"] else "event-only"
        out["health"] = HEALTH.get(out["health"], "unknown")
        out["backend"] = BACKENDS.get(out["backend"], "unknown")
        out["supervised"] = bool(out["supervised"])
        return out

    def list_events(self) -> list:
        data = self.request(MSG_LIST)
        got_type, _ = parse_header(data)
        if got_type != MSG_LIST_REPLY:
            raise WireError("unexpected reply type 0x%02x" % got_type)
        body = data[HDR.size:]
        count = struct.unpack_from("<I", body, 0)[0]
        events = []
        for i in range(count):
            off = 4 + i * EVENT_LINE.size
            if off + EVENT_LINE.size > len(body):
                raise WireError("truncated event list at index %d" % i)
            (event_id, state, locked, complete, reason, trig_ts,
             seq_emerg, seg, blk) = EVENT_LINE.unpack(
                 body[off:off + EVENT_LINE.size])
            events.append({
                "event_id": event_id,
                "state": STATES.get(state, "unknown"),
                "locked": bool(locked),
                "file_complete": bool(complete),
                "trig_reason": REASONS.get(reason, "unknown"),
                "trig_ts_us": trig_ts,
                "start_seg": seg, "start_block": blk,
                "seq_emerg": seq_emerg,
            })
        return events

    def verify_event(self, event_id: int) -> dict:
        body = struct.pack("<I", event_id)
        data = self.request(MSG_VERIFY, body, flags=FLAG_LOG_READ)
        got_type, _ = parse_header(data)
        if got_type != MSG_VERIFY_REPLY:
            raise WireError("unexpected reply type 0x%02x" % got_type)
        eid, blocks, fingerprint, complete, locked = VERIFY_BODY.unpack(
            data[HDR.size:HDR.size + VERIFY_BODY.size])
        return {"event_id": eid, "block_count": blocks,
                "fingerprint": "%08x" % fingerprint,
                "file_complete": bool(complete), "locked": bool(locked)}

    def export_event(self, event_id: int, max_pages: int = 4096) -> bytes:
        """Paginated raw-block export (same protocol as `edrctl export`)."""
        chunks, first = [], 0
        for _ in range(max_pages):
            body = struct.pack("<II", event_id, first)
            data = self.request(MSG_EXPORT, body, flags=FLAG_LOG_READ)
            got_type, _ = parse_header(data)
            if got_type != MSG_EXPORT_REPLY:
                raise WireError("unexpected reply type 0x%02x" % got_type)
            eid, first_blk, count, total = EXPORT_HDR.unpack(
                data[HDR.size:HDR.size + EXPORT_HDR.size])
            payload = data[HDR.size + EXPORT_HDR.size:]
            if len(payload) < count * BLOCK_SIZE:
                raise WireError("truncated export reply")
            chunks.append(payload[:count * BLOCK_SIZE])
            first += count
            if total == 0 or first >= total:
                return b"".join(chunks)
        raise WireError("export never completed after %d pages" % max_pages)


def connect_when_ready(sock_path: str, deadline_s: float,
                       timeout: float = 1.0):
    """Bounded wait for a listening journal; returns an open JournalWire."""
    limit = time.monotonic() + deadline_s
    last = None
    while time.monotonic() < limit:
        try:
            wire = JournalWire(sock_path, timeout=timeout).connect()
            wire.status()               # the service must answer, not just bind
            return wire
        except (OSError, WireError) as exc:
            last = exc
            time.sleep(0.002)
    raise WireError("journal not ready on %s within %.1f s (%s)"
                    % (sock_path, deadline_s, last))


# -- the real edrctl binary, when it can answer ------------------------------

def find_edrctl(bins_dir: str):
    """Path of the service tool in bins_dir (edr-ctl or edrctl), or None."""
    for name in ("edr-ctl", "edrctl"):
        path = os.path.join(bins_dir, name)
        if os.access(path, os.X_OK):
            return path
    return None


def run_edrctl(binary: str, sock: str, args, timeout: float = 10.0):
    """Run the real edrctl; returns (rc, stdout, stderr)."""
    cmd = [binary, "--journal", sock] + list(args)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr
    except (subprocess.TimeoutExpired, OSError) as exc:
        return 124, "", str(exc)


def edrctl_works(binary, sock: str) -> bool:
    """True when the real binary answers status --json (it may not: the
    portability layer lacks ambre_port_conn_request, see module docstring)."""
    if binary is None:
        return False
    rc, out, _ = run_edrctl(binary, sock, ["status", "--json"], timeout=5.0)
    if rc != 0:
        return False
    try:
        json.loads(out.strip().splitlines()[-1])
        return True
    except (ValueError, IndexError):
        return False
