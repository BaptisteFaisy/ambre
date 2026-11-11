#!/usr/bin/env python3
"""Load generators for the Ambre latency campaign (cahier des charges 7.5).

Three loads, each one real OS process (not a thread, not a simulation), each
self-terminating so no orphan can survive the campaign:

  cpu  : N worker processes at nice 19 spinning on arithmetic.  The CPU is
         saturated by tasks that are *less* priority than the recorder, which
         is the situation of spec 7.5 (a) "processeur sature par des taches
         moins prioritaires".
  disk : a writer process at nice 19 that writes and fdatasync()s bounded
         files in the archive directory.  The device and the flush path are
         busy while the recorder writes its journal: spec 7.5 (b) "ecritures
         intensives sur l'archive par un autre processus".
  mem  : a process at nice 19 that holds a large share of MemAvailable and
         keeps re-touching its pages, so the page cache is reclaimed under
         the writer: spec 7.5 (c) "pression memoire".

Every worker checks its own deadline, so even if the parent dies the load
stops by itself.  The parent additionally joins with a timeout and escalates
to terminate()/kill(), which makes the stop clean and deterministic.

Usage:
    python3 host/latency/load_gen.py --kind cpu  --seconds 30
    python3 host/latency/load_gen.py --kind disk --seconds 30 --dir /tmp/arch
    python3 host/latency/load_gen.py --kind mem  --seconds 30 --ratio 0.35
"""

from __future__ import annotations

import argparse
import multiprocessing
import os
import signal
import sys
import time

DEFAULT_DISK_BLOCK = 4 * 1024 * 1024     # 4 MiB per write cycle
DEFAULT_MEM_RATIO = 0.35                 # share of MemAvailable to hold
MIN_MEM_FLOOR = 2 * 1024 * 1024 * 1024   # never squeeze the machine below 2 GiB
NICE_LOW = 19                            # lowest priority, as spec 7.5 (a)


def mem_available_bytes():
    """MemAvailable of /proc/meminfo, in bytes (0 when unknown)."""
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        return 0
    return 0


def lower_priority():
    """Best-effort niceness; the campaign must run unprivileged."""
    try:
        os.nice(NICE_LOW)
    except OSError:
        pass


def install_clean_stop():
    """Turn SIGTERM into a SystemExit so `finally` cleanups always run."""
    def handler(_signum, _frame):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, handler)


# ------------------------------------------------------------------ workers --

def worker_cpu(seconds):
    """Spin on arithmetic until the deadline; the load is pure CPU."""
    lower_priority()
    install_clean_stop()
    deadline = time.monotonic() + seconds
    value = 0
    while time.monotonic() < deadline:
        for index in range(2000):
            value = (value * 31 + index) & 0xFFFFFFFF
    return value


def worker_disk(seconds, directory, block_bytes):
    """Write and fdatasync bounded files in `directory` until the deadline."""
    lower_priority()
    install_clean_stop()
    os.makedirs(directory, exist_ok=True)
    deadline = time.monotonic() + seconds
    path = os.path.join(directory, "load-disk.bin")
    chunk = b"\xa5" * (64 * 1024)
    written = 0
    try:
        while time.monotonic() < deadline:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            try:
                for _ in range(max(1, block_bytes // len(chunk))):
                    os.write(fd, chunk)
                os.fdatasync(fd)
                written += block_bytes
            finally:
                os.close(fd)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    return written


def worker_mem(seconds, nbytes):
    """Hold `nbytes` of anonymous memory and keep re-touching the pages."""
    lower_priority()
    install_clean_stop()
    deadline = time.monotonic() + seconds
    buffer = bytearray(nbytes)          # allocated and zeroed: pages resident
    stride = 4096
    offset = 0
    while time.monotonic() < deadline:
        buffer[offset] = (buffer[offset] + 1) & 0xFF
        offset = (offset + stride) % max(1, nbytes)
        time.sleep(0.02)
    del buffer
    return nbytes


# -------------------------------------------------------------- supervision --

def start_load(kind, seconds, workers=None, directory=None, ratio=None):
    """Start the load; returns (processes, description dict)."""
    procs = []
    description = {"kind": kind, "seconds": seconds}
    if kind == "cpu":
        count = workers or (os.cpu_count() or 2)
        for _ in range(count):
            proc = multiprocessing.Process(target=worker_cpu, args=(seconds,),
                                           daemon=True)
            proc.start()
            procs.append(proc)
        description["workers"] = count
        description["what"] = (f"{count} processes at nice {NICE_LOW} spinning "
                               f"on arithmetic for {seconds} s")
    elif kind == "disk":
        target = directory or os.path.join(os.getcwd(), "load-disk")
        proc = multiprocessing.Process(target=worker_disk,
                                       args=(seconds, target,
                                             DEFAULT_DISK_BLOCK), daemon=True)
        proc.start()
        procs.append(proc)
        description["directory"] = target
        description["block_bytes"] = DEFAULT_DISK_BLOCK
        description["what"] = (f"1 process at nice {NICE_LOW} writing "
                               f"{DEFAULT_DISK_BLOCK // (1024 * 1024)} MiB files "
                               f"and fdatasync()ing them in {target}")
    elif kind == "mem":
        available = mem_available_bytes()
        wanted = int((ratio if ratio is not None else DEFAULT_MEM_RATIO) *
                     available)
        wanted = max(0, min(wanted, max(0, available - MIN_MEM_FLOOR)))
        proc = multiprocessing.Process(target=worker_mem, args=(seconds, wanted),
                                       daemon=True)
        proc.start()
        procs.append(proc)
        description["held_bytes"] = wanted
        description["mem_available_bytes"] = available
        description["what"] = (f"1 process at nice {NICE_LOW} holding "
                               f"{wanted // (1024 * 1024)} MiB of anonymous "
                               "memory and re-touching it")
    else:
        description["what"] = "no load"
    return procs, description


def stop_load(procs, grace=5.0):
    """Stop the load and wait for it: no orphan may survive."""
    for proc in procs:
        if proc.is_alive():
            proc.terminate()
    deadline = time.monotonic() + grace
    for proc in procs:
        remaining = max(0.1, deadline - time.monotonic())
        proc.join(remaining)
    for proc in procs:
        if proc.is_alive():
            try:
                os.kill(proc.pid, signal.SIGKILL)
            except OSError:
                pass
            proc.join(1.0)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--kind", required=True, choices=["cpu", "disk", "mem"])
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--workers", type=int, default=None,
                        help="cpu load: number of worker processes")
    parser.add_argument("--dir", default=None,
                        help="disk load: directory where the files are written")
    parser.add_argument("--ratio", type=float, default=None,
                        help="mem load: share of MemAvailable to hold")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if args.seconds < 1:
        print("--seconds must be >= 1", file=sys.stderr)
        return 2
    procs, description = start_load(args.kind, args.seconds, args.workers,
                                    args.dir, args.ratio)
    print(f"load {args.kind} started: {description['what']}")
    try:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            time.sleep(min(5.0, max(0.1, deadline - time.monotonic())))
    except KeyboardInterrupt:
        pass
    finally:
        stop_load(procs)
    print(f"load {args.kind} stopped: {len(procs)} process(es), no orphan left")
    return 0


if __name__ == "__main__":
    sys.exit(main())
