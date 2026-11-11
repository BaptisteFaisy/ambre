"""Faulty block server with a simulated volatile cache (spec 7.4, extreme).

The guest disk is served over the network by this small NBD server instead of
being a plain host file, so it behaves like a real disk with a write-back
cache: writes land in an in-memory overlay and are **not** durable yet, a
FLUSH order makes them durable (written to the backing file and fsynced), and
at a power cut each write still in the overlay is **dropped** with probability
``--drop-unflushed-ratio``.  Every FLUSH order received is journalled to
``--log`` (JSONL), which proves that QNX emits flush orders for raw partitions
(essay E1).

This lifts the known limitation of killing QEMU (spec 7.4): data already
handed to the host survives a SIGKILL, so level 2 alone cannot lose a cache.
Here the cache is simulated, so the loss is real.

Protocol: standard NBD fixed-newstyle handshake (``NBD_OPT_EXPORT_NAME`` and
``NBD_OPT_GO``), then simple transmission with READ, WRITE, FLUSH, TRIM, DISC.
See PROTOCOL.md.  Standalone::

    python3 faulty_block_server.py --listen unix:/tmp/ambre.sock \
        --backing disk.img --drop-unflushed-ratio 0.5 --log flushes.jsonl

English only, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import signal
import socket
import struct
import sys
import threading
import time

NBDMAGIC = b"NBDMAGIC"
IHAVEOPT = b"IHAVEOPT"
NBD_REP_MAGIC = 0x0003E889045565A9
NBD_REQUEST_MAGIC = 0x25609513
NBD_SIMPLE_REPLY_MAGIC = 0x67446698

NBD_FLAG_FIXED_NEWSTYLE = 1 << 0
NBD_FLAG_NO_ZEROES = 1 << 1
NBD_FLAG_C_FIXED_NEWSTYLE = 1 << 0
NBD_FLAG_C_NO_ZEROES = 1 << 1

NBD_OPT_EXPORT_NAME = 1
NBD_OPT_ABORT = 2
NBD_OPT_INFO = 6
NBD_OPT_GO = 7
NBD_REP_ACK = 1
NBD_REP_INFO = 3
NBD_REP_ERR_UNSUP = 0x80000003
NBD_INFO_EXPORT = 0

NBD_CMD_READ = 0
NBD_CMD_WRITE = 1
NBD_CMD_DISC = 2
NBD_CMD_FLUSH = 3
NBD_CMD_TRIM = 4
NBD_CMD_FLAG_FUA = 1 << 0

NBD_FLAG_HAS_FLAGS = 1 << 0
NBD_FLAG_SEND_FLUSH = 1 << 2
NBD_FLAG_SEND_FUA = 1 << 3
NBD_FLAG_SEND_TRIM = 1 << 5
TRANSMISSION_FLAGS = (NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH
                      | NBD_FLAG_SEND_FUA | NBD_FLAG_SEND_TRIM)

SECTOR_SIZE = 512
DEFAULT_SIZE = 1 << 20


# -- event journal ---------------------------------------------------------

class EventLog:
    """Append-only JSONL journal of FLUSH orders and power cuts."""

    def __init__(self, path=None):
        self.path = path
        self.seq = 0
        self.lock = threading.Lock()
        if path:
            with open(path, "w"):
                pass  # truncate: one campaign run per file

    def record(self, event, **fields):
        """Append one entry; returns it so callers can report the outcome."""
        with self.lock:
            self.seq += 1
            entry = {"seq": self.seq, "event": event, "time": round(time.time(), 6)}
            entry.update(fields)
            if self.path:
                with open(self.path, "a") as handle:
                    handle.write(json.dumps(entry, sort_keys=True) + "\n")
            return entry


# -- the volatile cache ----------------------------------------------------

class VolatileCache:
    """Sector-granular overlay: writes are volatile until flushed."""

    def __init__(self, backing_path, size=None, sector_size=SECTOR_SIZE,
                 drop_ratio=0.0, rng=None, log=None):
        self.path, self.sector_size = backing_path, sector_size
        self.drop_ratio, self.rng = float(drop_ratio), rng or random.Random(20261111)
        self.log = log or EventLog(None)
        self.overlay = {}  # sector index -> bytearray(sector_size)
        self.lock, self.flush_count, self.dropped_total = threading.Lock(), 0, 0
        if not os.path.exists(backing_path):
            with open(backing_path, "wb") as handle:
                handle.truncate(DEFAULT_SIZE if size is None else size)
        self.size = os.path.getsize(backing_path) if size is None else int(size)

    # -- geometry ---------------------------------------------------------

    def _sectors(self, offset, length):
        """Inclusive sector index range covered by (offset, length)."""
        return offset // self.sector_size, (offset + length - 1) // self.sector_size

    def _sector_bytes(self, index):
        """Current content of one sector (overlay first, backing second)."""
        if index in self.overlay:
            return bytearray(self.overlay[index])
        base = index * self.sector_size
        with open(self.path, "rb") as handle:
            handle.seek(base)
            data = handle.read(self.sector_size)
        return bytearray(data.ljust(self.sector_size, b"\0"))

    # -- data path --------------------------------------------------------

    def read(self, offset, length):
        """Read ``length`` bytes at ``offset`` (overlay wins over backing)."""
        with self.lock:
            first, last = self._sectors(offset, length)
            buffer = b"".join(bytes(self._sector_bytes(index))
                              for index in range(first, last + 1))
            start = offset - first * self.sector_size
            return buffer[start:start + length]

    def write(self, offset, data):
        """Buffer a write in the volatile overlay (not durable yet)."""
        with self.lock:
            position = 0
            while position < len(data):
                absolute = offset + position
                index, within = divmod(absolute, self.sector_size)
                chunk = min(self.sector_size - within, len(data) - position)
                sector = self._sector_bytes(index)
                sector[within:within + chunk] = data[position:position + chunk]
                self.overlay[index] = sector
                position += chunk
            return len(data)

    def flush(self):
        """Make the whole overlay durable; returns the flushed runs."""
        with self.lock:
            runs = self._persist(self.overlay.keys())
            self.overlay.clear()
            self.flush_count += 1
            return self.log.record("flush", flush_count=self.flush_count, runs=runs,
                                   bytes=sum(r["length"] for r in runs),
                                   offset=runs[0]["offset"] if runs else None)

    def power_cut(self, ratio=None):
        """Simulate the cut: drop a fraction of the still-unflushed writes."""
        ratio = self.drop_ratio if ratio is None else float(ratio)
        with self.lock:
            kept, dropped = [], []
            for index in sorted(self.overlay):
                (dropped if self.rng.random() < ratio else kept).append(index)
            runs = self._persist(kept)
            self.overlay.clear()
            self.dropped_total += len(dropped) * self.sector_size
            return self.log.record(
                "power_cut", drop_ratio=ratio, kept_sectors=len(kept),
                dropped_sectors=len(dropped), kept_runs=runs,
                dropped_bytes=len(dropped) * self.sector_size)

    def _persist(self, sector_indexes):
        """Write the given sectors to the backing file, coalescing runs."""
        indexes = sorted(sector_indexes)
        if not indexes:
            return []
        runs, start, previous = [], indexes[0], indexes[0]
        for index in indexes[1:] + [None]:
            if index is not None and index == previous + 1:
                previous = index
                continue
            runs.append({"offset": start * self.sector_size,
                         "length": (previous - start + 1) * self.sector_size})
            start = previous = index if index is not None else previous
        with open(self.path, "r+b") as handle:
            for run in runs:
                first = run["offset"] // self.sector_size
                count = run["length"] // self.sector_size
                payload = b"".join(bytes(self.overlay[first + step])
                                   for step in range(count))
                handle.seek(run["offset"])
                handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        return runs


# -- NBD server ------------------------------------------------------------

def recv_exactly(conn, count):
    """Read exactly ``count`` bytes from ``conn``, or raise ConnectionError."""
    buffer = b""
    while len(buffer) < count:
        chunk = conn.recv(count - len(buffer))
        if not chunk:
            raise ConnectionError("client closed")
        buffer += chunk
    return buffer


class NbdServer:
    """Threaded NBD server exposing :class:`VolatileCache` as one export."""

    def __init__(self, cache, log=None, listen="unix:/tmp/ambre-nbd.sock",
                 export_name="ambre"):
        self.cache, self.log, self.listen = cache, log or cache.log, listen
        self.export_name, self.sock, self.address = export_name, None, None
        self.running, self.thread, self.clients = False, None, []
        self.flush_orders, self.lock = 0, threading.Lock()

    def start(self):
        """Bind, listen and serve in a background thread."""
        self.sock = self._make_socket()
        self.sock.listen(8)
        self.sock.settimeout(0.2)  # so the accept loop can see `running` go False
        self.running = True
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()
        return self

    def _make_socket(self):
        if self.listen.startswith("unix:"):
            path = self.listen.split(":", 1)[1]
            if os.path.exists(path):
                os.unlink(path)
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.bind(path)
            self.address = path
            return sock
        host, _, port = self.listen.rpartition(":")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((host or "127.0.0.1", int(port)))
        self.address = "%s:%d" % sock.getsockname()[:2]
        return sock

    def _accept_loop(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.clients.append(conn)
            threading.Thread(target=self._serve_client, args=(conn,), daemon=True).start()

    def _serve_client(self, conn):
        try:
            if self._handshake(conn):
                self._transmission(conn)
        except (ConnectionError, OSError, ValueError, struct.error):
            pass  # a client going away is normal (guest killed at the cut)
        finally:
            try:
                conn.close()
            except OSError:
                pass
            with self.lock:
                if conn in self.clients:
                    self.clients.remove(conn)

    def _handshake(self, conn):
        """Fixed-newstyle handshake; returns True when transmission starts."""
        conn.sendall(NBDMAGIC + IHAVEOPT
                     + struct.pack(">H", NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES))
        client_flags = struct.unpack(">I", recv_exactly(conn, 4))[0]
        no_zeroes = bool(client_flags & NBD_FLAG_C_NO_ZEROES)
        option_magic = int.from_bytes(IHAVEOPT, "big")
        while True:
            magic, option, length = struct.unpack(">QII", recv_exactly(conn, 16))
            if magic != option_magic:
                raise ValueError("bad option magic")
            if length:
                recv_exactly(conn, length)  # export name / payload, unused
            if option == NBD_OPT_EXPORT_NAME:
                conn.sendall(struct.pack(">QH", self.cache.size, TRANSMISSION_FLAGS)
                             + (b"" if no_zeroes else b"\0" * 124))
                return True
            if option in (NBD_OPT_GO, NBD_OPT_INFO):
                info = struct.pack(">HQH", NBD_INFO_EXPORT, self.cache.size,
                                   TRANSMISSION_FLAGS)
                self._option_reply(conn, option, NBD_REP_INFO, info)
                self._option_reply(conn, option, NBD_REP_ACK, b"")
                if option == NBD_OPT_GO:
                    return True
                continue
            if option == NBD_OPT_ABORT:
                self._option_reply(conn, option, NBD_REP_ACK, b"")
                return False
            self._option_reply(conn, option, NBD_REP_ERR_UNSUP, b"")

    @staticmethod
    def _option_reply(conn, option, reply_type, payload):
        conn.sendall(struct.pack(">QIII", NBD_REP_MAGIC, option, reply_type,
                                 len(payload)) + payload)

    def _transmission(self, conn):
        """Serve READ / WRITE / FLUSH / TRIM / DISC requests."""
        while True:
            magic, flags, command, handle, offset, length = struct.unpack(
                ">IHHQQI", recv_exactly(conn, 28))
            if magic != NBD_REQUEST_MAGIC:
                raise ValueError("bad request magic")
            if command == NBD_CMD_DISC:
                return
            data = recv_exactly(conn, length) if command == NBD_CMD_WRITE else b""
            error, payload = self._execute(command, flags, offset, length, data)
            conn.sendall(struct.pack(">IIQ", NBD_SIMPLE_REPLY_MAGIC, error, handle)
                         + payload)

    def _execute(self, command, flags, offset, length, data):
        """Run one command; returns (nbd error, reply payload)."""
        if offset + length > self.cache.size:
            return 22, b""  # EINVAL: out of the export
        if command == NBD_CMD_READ:
            return 0, self.cache.read(offset, length)
        if command == NBD_CMD_WRITE:
            self.cache.write(offset, data)
            if not flags & NBD_CMD_FLAG_FUA:
                return 0, b""
            command = NBD_CMD_FLUSH  # FUA = write + flush
        if command == NBD_CMD_FLUSH:
            self.cache.flush()
            self._count_flush()
            return 0, b""
        if command == NBD_CMD_TRIM:
            self.cache.write(offset, b"\0" * length)
            return 0, b""
        return 22, b""

    def _count_flush(self):
        with self.lock:
            self.flush_orders += 1

    # -- cut and shutdown --------------------------------------------------

    def power_cut(self, ratio=None):
        """Drop a fraction of the unflushed writes, as a power cut would."""
        return self.cache.power_cut(ratio)

    def stop(self):
        """Stop accepting, drop clients and release the listening socket."""
        self.running = False
        try:
            self.sock.close()
        except (AttributeError, OSError):
            pass
        with self.lock:
            for conn in list(self.clients):
                try:
                    conn.close()
                except OSError:
                    pass
            self.clients = []
        if self.thread is not None:
            self.thread.join(timeout=5)
        if self.listen.startswith("unix:"):
            try:
                os.unlink(self.listen.split(":", 1)[1])
            except OSError:
                pass


# -- standalone entry point ------------------------------------------------

def make_server(backing, listen="unix:/tmp/ambre-nbd.sock", size=None,
                drop_ratio=0.0, log_path=None, seed=20261111,
                export_name="ambre"):
    """Build the cache + server pair (not started yet)."""
    log = EventLog(log_path)
    cache = VolatileCache(backing, size=size, drop_ratio=drop_ratio, log=log,
                          rng=random.Random(seed))
    return NbdServer(cache, log=log, listen=listen, export_name=export_name)


def start_server(backing, listen="unix:/tmp/ambre-nbd.sock", size=None,
                 drop_ratio=0.0, log_path=None, seed=20261111):
    """Factory shared by the E1 probe and the test suite (already serving)."""
    return make_server(backing, listen, size, drop_ratio, log_path, seed).start()


def build_server(args):
    """Create the cache + server pair from parsed command-line arguments."""
    return make_server(args.backing, args.listen, args.size, args.drop,
                       args.log, args.seed, args.export)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Faulty NBD block server (Ambre)")
    parser.add_argument("--listen", default="unix:/tmp/ambre-nbd.sock",
                        help="unix:/path or host:port")
    parser.add_argument("--backing", default="ambre-nbd-backing.img")
    parser.add_argument("--size", type=int, default=None, help="export size (bytes)")
    parser.add_argument("--drop-unflushed-ratio", "--drop", dest="drop", type=float,
                        default=0.5, help="probability an unflushed write is lost")
    parser.add_argument("--seed", type=int, default=20261111)
    parser.add_argument("--export", default="ambre", help="NBD export name")
    parser.add_argument("--log", default=None, help="JSONL journal of FLUSH orders")
    args = parser.parse_args(argv)

    server = build_server(args)

    def on_signal(_signum, _frame):
        """SIGTERM/SIGINT = power cut: drop a fraction of unflushed writes."""
        stats = server.power_cut()
        print("power cut: %d sector(s) kept, %d dropped"
              % (stats["kept_sectors"], stats["dropped_sectors"]))
        server.stop()
        sys.exit(0)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    server.start()
    print("faulty block server listening on %s (export '%s', %d bytes)"
          % (server.address, args.export, server.cache.size))
    try:
        while True:
            time.sleep(0.2)
    except KeyboardInterrupt:
        on_signal(None, None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
