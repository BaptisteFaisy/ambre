"""Essay E1 — does a write + flush on a raw partition reach the virtual disk?

Question (spec section 10, E1): *from QNX under QEMU, does a write on a raw
partition followed by a flush order reach the virtual disk as a FLUSH?*
If not, fall back to a dedicated Power-Safe partition with ``fsync()`` and a
longer freeze window.

Two modes
---------
``--mode host`` (measurable today, no QNX needed)
    Starts :mod:`faulty_block_server` on a local socket, writes 4 KiB through
    the NBD client, sends FLUSH and checks that (a) the block server logged
    the FLUSH order and (b) the bytes are durable in the backing file.  It
    demonstrates the *measurement method* and yields a binary verdict plus the
    measured flush latency.  ``--fallback qemu-io`` uses ``qemu-io`` on a plain
    file when present (durability only: it cannot journal FLUSH orders).

``--mode guest`` (needs the QNX 8.0 SDP and a guest image)
    Boots the guest, runs the documented QNX commands below and asks the block
    server whether a FLUSH order arrived.  Until the SDP is available this
    mode reports ``pending`` and exits 2.

Reference NBD client
--------------------
:class:`NbdClient` is the test client for ``faulty_block_server.py`` (see
PROTOCOL.md); it is also what the test suite drives.

English only, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import faulty_block_server as fbs  # noqa: E402
import qemu_vm  # noqa: E402

# -- QNX guest side: exact commands, to run once the SDP is available -------
#
# Raw partition, QNX 8.0 (devb-* block driver, no filesystem in the way).
# `/dev/hd0` is the whole disk; writing at a 1 MiB offset keeps the partition
# table and the boot blocks untouched.
GUEST_RAW_DEVICE = "/dev/hd0"
GUEST_OFFSET = 1 << 20
GUEST_LENGTH = 4096

#: 1. write 4 KiB of known data straight to the raw device (dd on the block
#:    special file: no filesystem cache involved, only the driver cache).
GUEST_WRITE_CMD = ("dd if=/dev/urandom of=%s bs=512 seek=%d count=%d"
                   % (GUEST_RAW_DEVICE, GUEST_OFFSET // 512, GUEST_LENGTH // 512))

#: 2a. ask the block driver to flush its cache.  `sync` is the QNX utility that
#:     pushes cached blocks to the medium for every mounted driver; the
#:     devb-* drivers honour it.  TODO(qnx-sdp): confirm on the real image that
#:     `sync` issues NBD_CMD_FLUSH on a *raw* (unmounted) device; if it does
#:     not, use the dedicated-partition fallback of E1.
GUEST_FLUSH_CMD = "sync"

#: 2b. fallback path: fsync() on the open descriptor of the raw device.  This
#:     needs the small helper we ship with the EDR (to be built with the QNX
#:     toolchain).  TODO(qnx-sdp): build `edrctl fsync-test` and compare.
GUEST_FSYNC_CMD = ("edrctl fsync-test --device %s --offset %d --length %d"
                   % (GUEST_RAW_DEVICE, GUEST_OFFSET, GUEST_LENGTH))

#: 3. read back what the medium holds (through the cache: use `-d` to bypass
#:    the driver cache when checking durability).
GUEST_READBACK_CMD = ("dd if=%s bs=512 skip=%d count=%d | cksum"
                      % (GUEST_RAW_DEVICE, GUEST_OFFSET // 512, GUEST_LENGTH // 512))


# -- reference NBD client --------------------------------------------------

def _recv_exactly(conn, count):
    """Read exactly ``count`` bytes, or raise ConnectionError."""
    buffer = b""
    while len(buffer) < count:
        chunk = conn.recv(count - len(buffer))
        if not chunk:
            raise ConnectionError("server closed")
        buffer += chunk
    return buffer


class NbdClient:
    """Tiny NBD client: enough to write, read and flush for the probes."""

    def __init__(self, address, export_name="ambre"):
        self.address, self.export_name = address, export_name
        self.sock, self.handle, self.size = None, 0, 0

    def connect(self, timeout_s=5.0):
        """Connect and complete the handshake (EXPORT_NAME option)."""
        is_unix = self.address.startswith("unix:") or self.address.startswith("/")
        path = self.address.split(":", 1)[-1] if self.address.startswith("unix:") \
            else self.address
        self.sock = socket.socket(socket.AF_UNIX if is_unix else socket.AF_INET,
                                  socket.SOCK_STREAM)
        self.sock.settimeout(timeout_s)
        if is_unix:
            self.sock.connect(path)
        else:
            host, _, port = self.address.rpartition(":")
            self.sock.connect((host or "127.0.0.1", int(port)))
        self._handshake()
        return self

    def _handshake(self):
        if _recv_exactly(self.sock, 8) != fbs.NBDMAGIC:
            raise ValueError("not an NBD server")
        _recv_exactly(self.sock, 10)  # IHAVEOPT magic + handshake flags
        self.sock.sendall(struct.pack(">I", fbs.NBD_FLAG_C_FIXED_NEWSTYLE
                                      | fbs.NBD_FLAG_C_NO_ZEROES))
        name = self.export_name.encode()
        self.sock.sendall(struct.pack(">QII", int.from_bytes(fbs.IHAVEOPT, "big"),
                                      fbs.NBD_OPT_EXPORT_NAME, len(name)) + name)
        self.size, _flags = struct.unpack(">QH", _recv_exactly(self.sock, 10))

    def _request(self, command, offset=0, length=0, data=b"", flags=0):
        self.handle = (self.handle + 1) & 0xFFFFFFFFFFFFFFFF
        self.sock.sendall(struct.pack(">IHHQQI", fbs.NBD_REQUEST_MAGIC, flags, command,
                                      self.handle, offset, length) + data)
        magic, error, handle = struct.unpack(">IIQ", _recv_exactly(self.sock, 16))
        if magic != fbs.NBD_SIMPLE_REPLY_MAGIC or handle != self.handle:
            raise ValueError("bad NBD reply")
        if error:
            raise OSError("NBD error %d" % error)
        return _recv_exactly(self.sock, length) if command == fbs.NBD_CMD_READ else b""

    def write(self, offset, data):
        """Write (buffered in the server-side volatile cache)."""
        self._request(fbs.NBD_CMD_WRITE, offset, len(data), data)

    def read(self, offset, length):
        return self._request(fbs.NBD_CMD_READ, offset, length)

    def flush(self):
        """Send FLUSH; returns the measured round-trip latency in seconds."""
        started = time.monotonic()
        self._request(fbs.NBD_CMD_FLUSH)
        return time.monotonic() - started

    def disconnect(self):
        """Send DISC (best effort) and close the socket."""
        if self.sock is None:
            return
        try:
            self.sock.sendall(struct.pack(">IHHQQI", fbs.NBD_REQUEST_MAGIC, 0,
                                          fbs.NBD_CMD_DISC, 0, 0, 0))
            self.sock.close()
        except OSError:
            pass
        self.sock = None


# -- host mode -------------------------------------------------------------

def read_log(path):
    """Read the JSONL journal produced by the block server."""
    entries = []
    if not path or not os.path.exists(path):
        return entries
    with open(path) as handle:
        for line in handle:
            try:
                entries.append(json.loads(line))
            except ValueError:
                continue
    return entries


def probe_with_nbd(workdir):
    """Write + FLUSH through the faulty block server and verify durability."""
    backing = os.path.join(workdir, "e1-backing.img")
    log_path = os.path.join(workdir, "e1-flushes.jsonl")
    socket_path = os.path.join(workdir, "e1-nbd.sock")
    server = fbs.start_server(backing, listen="unix:" + socket_path,
                              size=GUEST_OFFSET * 2, drop_ratio=0.0,
                              log_path=log_path)
    client = None
    try:
        client = NbdClient("unix:" + socket_path).connect()
        payload = bytes((index * 7) & 0xFF for index in range(GUEST_LENGTH))
        client.write(GUEST_OFFSET, payload)
        latency = client.flush()
        with open(backing, "rb") as handle:
            durable = handle.read()
        on_medium = durable[GUEST_OFFSET:GUEST_OFFSET + GUEST_LENGTH] == payload
        readback = client.read(GUEST_OFFSET, GUEST_LENGTH)
    finally:
        if client is not None:
            client.disconnect()
        server.stop()
    entries = read_log(log_path)
    flushes = [entry for entry in entries if entry.get("event") == "flush"]
    return {
        "method": "nbd-faulty-block-server",
        "flush_orders_received": len(flushes),
        "flush_logged": bool(flushes),
        "flush_latency_s": round(latency, 6),
        "write_durable_after_flush": on_medium,
        "readback_matches": readback == payload,
        "log_entries": entries,
    }


def probe_with_qemu_io(workdir):
    """Fallback: qemu-io on a plain file (durability only, no FLUSH journal)."""
    qemu_io = None
    for candidate in ("qemu-io", "/usr/bin/qemu-io"):
        if os.path.exists(candidate) or qemu_vm.run_capture(["which", candidate]).ok:
            qemu_io = candidate
            break
    if not qemu_io:
        return {"method": "qemu-io", "available": False,
                "verdict": "inconclusive", "reason": "qemu-io not installed"}
    image = os.path.join(workdir, "e1-qemu-io.img")
    with open(image, "wb") as handle:
        handle.truncate(GUEST_OFFSET * 2)
    command = ["%s" % qemu_io, "-f", "raw", "-t", "none",
               "-c", "write -P 0xab %d %d" % (GUEST_OFFSET, GUEST_LENGTH),
               "-c", "flush", image]
    started = time.monotonic()
    result = qemu_vm.run_capture(command, timeout_s=30.0)
    elapsed = time.monotonic() - started
    with open(image, "rb") as handle:
        handle.seek(GUEST_OFFSET)
        data = handle.read(GUEST_LENGTH)
    return {"method": "qemu-io", "available": True, "returncode": result.returncode,
            "flush_latency_s": round(elapsed, 6),
            "write_durable_after_flush": data == b"\xab" * GUEST_LENGTH,
            "flush_logged": None,
            "note": "qemu-io cannot journal FLUSH orders: durability only",
            "stderr": result.stderr.strip()[:400]}


# -- guest mode ------------------------------------------------------------

def guest_command_plan():
    """The exact commands to run in the QNX guest once the SDP is available."""
    return [
        {"step": 1, "why": "write 4 KiB on the raw partition",
         "command": GUEST_WRITE_CMD},
        {"step": 2, "why": "ask the block driver to flush its cache",
         "command": GUEST_FLUSH_CMD},
        {"step": 2.1, "why": "fallback: fsync() on the raw descriptor",
         "command": GUEST_FSYNC_CMD},
        {"step": 3, "why": "read back what the medium holds",
         "command": GUEST_READBACK_CMD},
    ]


def probe_guest(args, workdir):
    """Run E1 inside the guest, with the disk served by the block server."""
    if not args.image:
        return {"method": "guest-qnx", "available": False, "verdict": "pending",
                "reason": "QNX 8.0 image not available on this host",
                "commands": guest_command_plan()}
    backing = os.path.join(workdir, "e1-guest-backing.img")
    log_path = os.path.join(workdir, "e1-guest-flushes.jsonl")
    socket_path = os.path.join(workdir, "e1-guest.sock")
    with open(args.image, "rb") as source, open(backing, "wb") as target:
        target.write(source.read())
    server = fbs.start_server(backing, listen="unix:" + socket_path,
                              size=None, drop_ratio=0.0, log_path=log_path)
    vm = qemu_vm.QemuVM(image=None, backend="qemu", workdir=workdir,
                        nbd_socket=socket_path, boot_timeout_s=args.boot_timeout)
    try:
        vm.start()
        for step in guest_command_plan()[:2]:
            vm.run_guest(step["command"], timeout_s=60.0)
        entries = read_log(log_path)
    finally:
        vm.stop()
        server.stop()
    flushes = [entry for entry in entries if entry.get("event") == "flush"]
    return {"method": "guest-qnx", "available": True,
            "flush_orders_received": len(flushes),
            "flush_logged": bool(flushes), "log_entries": entries,
            "commands": guest_command_plan()}


# -- reporting -------------------------------------------------------------

def verdict_of(result):
    """Binary verdict from a probe result: flush reaches the medium or not."""
    if result.get("verdict") == "pending":
        return "pending"
    if result.get("flush_logged") is True and result.get("write_durable_after_flush"):
        return "flush-reaches-medium"
    if result.get("flush_logged") is False:
        return "flush-not-seen"
    return "inconclusive"


def main(argv=None):
    parser = argparse.ArgumentParser(description="Essay E1: raw write + flush probe")
    parser.add_argument("--mode", choices=("host", "guest"), default="host")
    parser.add_argument("--fallback", choices=("nbd", "qemu-io"), default="nbd",
                        help="host mode: NBD block server (default) or qemu-io")
    parser.add_argument("--image", default=None, help="guest disk image (guest mode)")
    parser.add_argument("--boot-timeout", type=float,
                        default=qemu_vm.DEFAULT_BOOT_TIMEOUT_S)
    parser.add_argument("--report", default=None, help="write the result as JSON")
    args = parser.parse_args(argv)
    workdir = tempfile.mkdtemp(prefix="ambre-e1-")
    try:
        if args.mode == "guest":
            result = probe_guest(args, workdir)
        elif args.fallback == "qemu-io":
            result = probe_with_qemu_io(workdir)
        else:
            result = probe_with_nbd(workdir)
    finally:
        import shutil
        shutil.rmtree(workdir, ignore_errors=True)
    result["verdict"] = verdict_of(result)
    if args.report:
        with open(args.report, "w") as handle:
            json.dump(result, handle, indent=2, sort_keys=True)
            handle.write("\n")
    print("E1 flush probe — mode=%s method=%s" % (args.mode, result.get("method")))
    for key in ("verdict", "flush_orders_received", "flush_latency_s",
                "write_durable_after_flush", "readback_matches", "reason", "note"):
        if key in result:
            print("  %-26s: %s" % (key, result[key]))
    if result.get("verdict") == "flush-reaches-medium":
        print("  => flush arrives at the virtual disk: E1 is satisfied")
        return 0
    if result.get("verdict") == "pending":
        print("  => pending: run again with --mode guest once the QNX image exists")
        return 2
    if result.get("verdict") == "inconclusive":
        print("  => inconclusive: durability seen, but no FLUSH order was journalled")
        print("     use the NBD block server (default) to observe FLUSH orders")
        return 2
    print("  => E1 not satisfied with this method; see the fallback in the spec")
    return 1


if __name__ == "__main__":
    sys.exit(main())
