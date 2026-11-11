"""QEMU virtual machine control for the Ambre power-cut campaign (spec 7.4).

Single place where "how do we drive a guest machine" lives: the campaign
orchestrator, the E1 flush probe and the E3 cycle timer all use
:class:`QemuVM` and the helpers below, so probe/wait/report logic is never
duplicated.

``qemu`` backend: real ``qemu-system-x86_64`` line — guest disk with
``cache=none`` and ``aio=native`` (ordered writes), optional ``blkdebug:``
error injection, optional NBD drive served by ``faulty_block_server.py``, no
network (``-net none``: closed-loop EDR, VIE-03), serial console on a unix
socket, QMP socket.  ``dry-run`` backend: a generated fake-VM script replaces
QEMU — it publishes the events a real guest would have acquired before the cut
(minus a configurable "not yet durable" window) and dies when killed, so all
logic is testable today without a QNX 8.0 image.

``power_cut()`` sends SIGKILL: the guest stops instantly, as if power had been
removed. Known limitation (spec 7.4): writes already handed to the host
survive; this level tests ordering and interruption, not the loss of a volatile
cache. ``faulty_block_server.py`` lifts that limitation.
"""

from __future__ import annotations

import atexit
import json
import os
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time

DEFAULT_LIST_CMD = "edrctl list --json"
DEFAULT_EXPORT_CMD = "edrctl export"
DEFAULT_PROMPT = "#"
DEFAULT_BOOT_TIMEOUT_S = 120.0
DEFAULT_QEMU = "qemu-system-x86_64"

#: Live VMs, stopped at interpreter exit so no orphan process is left behind.
_LIVE_VMS = []


class PowercutError(RuntimeError):
    """Unrecoverable error while controlling a virtual machine."""


class GuestResult:
    """Outcome of a command executed inside the guest."""

    def __init__(self, returncode, stdout="", stderr=""):
        self.returncode, self.stdout, self.stderr = int(returncode), stdout or "", stderr or ""

    @property
    def ok(self):
        return self.returncode == 0

    def __repr__(self):
        return "GuestResult(rc=%d, stdout=%r)" % (self.returncode, self.stdout[:120])


# -- shared helpers (used by every tool of the power-cut level) -------------

def wait_for(predicate, timeout_s, poll_s=0.02, description="condition"):
    """Poll ``predicate`` until true; bounded by ``timeout_s`` (raises)."""
    deadline = time.monotonic() + max(0.0, timeout_s)
    while True:
        if predicate():
            return True
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("timeout after %.1fs waiting for %s"
                               % (timeout_s, description))
        time.sleep(min(poll_s, remaining))


def sleep_slices(seconds, slice_s=0.005):
    """Sleep ``seconds`` in short slices; never one long fixed sleep."""
    deadline = time.monotonic() + max(0.0, seconds)
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(remaining, slice_s))


def run_capture(argv, timeout_s=30.0, input_text=None):
    """Run ``argv``, capture output; never raises on timeout."""
    try:
        proc = subprocess.run(argv, input=input_text, capture_output=True,
                              text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout if isinstance(exc.stdout, str) else ""
        return GuestResult(124, out, "timeout after %.1fs" % timeout_s)
    except OSError as exc:
        return GuestResult(127, "", str(exc))
    return GuestResult(proc.returncode, proc.stdout, proc.stderr)


def terminate_process(proc, timeout_s=5.0):
    """Stop ``proc`` politely, then forcibly.  True when it is gone."""
    if proc is None or proc.poll() is not None:
        return True
    try:
        proc.terminate()
        proc.wait(timeout=timeout_s)
        return True
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        proc.kill()
        proc.wait(timeout=timeout_s)
    except (OSError, subprocess.TimeoutExpired):
        pass
    return proc.poll() is not None


def parse_events(payload):
    """Parse a guest export payload into a list of event dicts (tolerant)."""
    text = (payload or "").strip()
    if not text:
        return []
    try:
        data = json.loads(text)
    except ValueError:
        return []
    if isinstance(data, list):
        items = data
    elif isinstance(data, dict):
        items = [data]
        for key in ("events", "records", "items", "data"):
            if isinstance(data.get(key), list):
                items = data[key]
                break
    else:
        return []
    return [item for item in items if isinstance(item, dict)]


def stats(values):
    """min / median / p99 / max / mean of a sample list (None when empty)."""
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(0.99 * (len(ordered) - 1))))
    return {"count": len(ordered), "min": round(ordered[0], 4),
            "median": round(statistics.median(ordered), 4),
            "p99": round(ordered[index], 4), "max": round(ordered[-1], 4),
            "mean": round(statistics.fmean(ordered), 4)}


def histogram_text(values, title, buckets=8):
    """Render a plain-text histogram (no third-party plotting library)."""
    if not values:
        return "%s: no samples\n" % title
    low, high = min(values), max(values)
    if high <= low:
        return "%s: all %d samples = %.3fs\n" % (title, len(values), low)
    width = (high - low) / buckets
    counts = [0] * buckets
    for value in values:
        counts[min(buckets - 1, int((value - low) / width))] += 1
    peak = max(counts) or 1
    lines = ["%s (%d samples)" % (title, len(values))]
    for index, count in enumerate(counts):
        start = low + index * width
        lines.append("  %7.3f-%7.3f s | %-40s %d"
                     % (start, start + width, "#" * int(round(40.0 * count / peak)),
                        count))
    return "\n".join(lines) + "\n"


# -- guest transport: serial console on a unix socket (real QEMU) ----------

class SerialConsole:
    """Send a command on the guest serial line, read back to the prompt."""

    def __init__(self, sock_path, prompt=DEFAULT_PROMPT):
        self.sock_path = sock_path
        self.prompt = prompt
        self.sock = None

    def connect(self, timeout_s=10.0):
        """Connect to the console socket, retrying within ``timeout_s``."""
        def attempt():
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(self.sock_path)
            except OSError:
                sock.close()
                return False
            self.sock = sock
            return True
        try:
            return wait_for(attempt, timeout_s, 0.05, "serial console socket")
        except TimeoutError:
            return False

    def run(self, command, timeout_s=30.0):
        if self.sock is None and not self.connect(min(timeout_s, 10.0)):
            return GuestResult(1, "", "serial console not available")
        try:
            self.sock.sendall((command + "\r\n").encode("utf-8", "replace"))
        except OSError as exc:
            return GuestResult(1, "", str(exc))
        return GuestResult(0, self._read_until_prompt(timeout_s), "")

    def _read_until_prompt(self, timeout_s):
        deadline = time.monotonic() + timeout_s
        chunks = []
        self.sock.settimeout(0.2)
        while time.monotonic() < deadline:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            chunks.append(data.decode("utf-8", "replace"))
            if "".join(chunks).rstrip().endswith(self.prompt):
                break
        return "".join(chunks)

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None  # idempotent: closing twice is harmless


# -- fake VM used by the dry-run backend -----------------------------------

FAKE_VM_SCRIPT = r'''#!/usr/bin/env python3
"""Fake QEMU stand-in for the dry-run backend (generated by qemu_vm.py).

Models a guest that booted, ran the scenario and was cut at --cut-ms: events
acquired at or before (cut - unflushed-window) are durable and published in the
guest journal; events acquired inside that window are lost, as a volatile
write-back cache would lose them.
"""
import argparse, json, os, time


def write_json(path, payload):
    with open(path + ".tmp", "w") as handle:
        json.dump(payload, handle)
    os.replace(path + ".tmp", path)


def read_json(path, default):
    try:
        with open(path) as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return default


def main():
    parser = argparse.ArgumentParser()
    for name, default, kind in (("--state", None, str), ("--journal", None, str),
                                ("--boot-counter", "", str),
                                ("--ready-delay", 0.05, float),
                                ("--event-plan", "[]", str),
                                ("--cut-ms", 0.0, float),
                                ("--unflushed-window-ms", 0.0, float)):
        parser.add_argument(name, default=default, type=kind, required=name in
                            ("--state", "--journal"))
    args = parser.parse_args()
    boots = int(read_json(args.boot_counter, {}).get("boots", 0)) + 1
    write_json(args.boot_counter, {"boots": boots})
    # Publish the events that reached the medium before the cut.
    journal = read_json(args.journal, {}).get("events", [])
    known = {event.get("id") for event in journal}
    for item in sorted(json.loads(args.event_plan), key=lambda e: e["at_ms"]):
        if item["at_ms"] <= args.cut_ms - args.unflushed_window_ms \
                and item["event"].get("id") not in known:
            journal.append(item["event"])
    write_json(args.journal, {"events": journal})
    started = time.monotonic()
    while (time.monotonic() - started) < args.ready_delay:
        time.sleep(0.002)
    write_json(args.state, {"ready": True, "pid": os.getpid(), "boots": boots,
                            "started_at": time.time()})
    while True:  # stay alive until SIGKILL: this is the running guest
        time.sleep(0.05)


if __name__ == "__main__":
    main()
'''


# -- the virtual machine ---------------------------------------------------

class QemuVM:
    """A guest machine that can be started, probed, cut and restarted."""

    def __init__(self, image=None, backend="qemu", mem_mb=512, cpus=2, workdir=None,
                 console_prompt=DEFAULT_PROMPT, boot_timeout_s=DEFAULT_BOOT_TIMEOUT_S,
                 blkdebug_config=None, nbd_socket=None, cache="none", aio="native",
                 extra_args=None, dry_run_ready_s=0.05, dry_run_event_plan=None,
                 dry_run_cut_ms=0.0, dry_run_unflushed_ms=0.0, keep_workdir=False):
        if backend not in ("qemu", "dry-run"):
            raise ValueError("unsupported backend: %r" % (backend,))
        self.image = image
        self.backend = backend
        self.mem_mb, self.cpus = mem_mb, cpus
        self.console_prompt = console_prompt
        self.boot_timeout_s = boot_timeout_s
        self.blkdebug_config = blkdebug_config
        self.nbd_socket = nbd_socket
        self.cache = cache
        self.aio = aio
        self.extra_args = list(extra_args or [])
        self.dry_run_ready_s = dry_run_ready_s
        self.dry_run_event_plan = list(dry_run_event_plan or [])
        self.dry_run_cut_ms, self.dry_run_unflushed_ms = dry_run_cut_ms, dry_run_unflushed_ms
        self.keep_workdir = keep_workdir
        self.workdir = workdir or tempfile.mkdtemp(prefix="ambre-powercut-")
        self._owns_workdir = workdir is None
        self.proc = None
        self.console = None
        self.boot_seconds = 0.0
        self.cut_seconds = 0.0
        self.serial_path = os.path.join(self.workdir, "console.sock")  # guest shell
        self.qmp_path = os.path.join(self.workdir, "qmp.sock")
        self.state_path = os.path.join(self.workdir, "guest-state.json")
        self.journal_path = os.path.join(self.workdir, "guest-journal.json")
        self.boot_counter_path = os.path.join(self.workdir, "boot-counter.json")
        _LIVE_VMS.append(self)  # tracked so a crash cannot orphan the machine

    # -- command line ------------------------------------------------------

    def qemu_binary(self):
        """Locate the qemu binary (``QEMU_BINARY`` wins, then PATH)."""
        candidate = os.environ.get("QEMU_BINARY")
        if candidate:
            return candidate
        found = shutil.which(DEFAULT_QEMU)
        if not found:
            raise PowercutError("%s not found in PATH" % DEFAULT_QEMU)
        return found

    def drive_spec(self):
        """Build the ``-drive`` argument value for the guest disk."""
        if self.nbd_socket:
            # Extreme level: the block server owns the volatile cache.
            return ("file=nbd:unix:%s,if=none,id=ambre,format=raw,cache=writeback"
                    % self.nbd_socket)
        if not self.image:
            raise PowercutError("no guest image given")
        if self.blkdebug_config:
            # Error injection: blkdebug:CONFIG:IMAGE, same cache/aio policy.
            return ("file=blkdebug:%s:%s,if=none,id=ambre,format=raw,cache=%s,aio=%s"
                    % (self.blkdebug_config, self.image, self.cache, self.aio))
        return ("file=%s,if=none,id=ambre,format=raw,cache=%s,aio=%s"
                % (self.image, self.cache, self.aio))

    def command_line(self):
        """Return the argv used to launch the machine."""
        if self.backend == "dry-run":
            return [sys.executable, self.fake_vm_path(),
                    "--state", self.state_path,
                    "--journal", self.journal_path,
                    "--boot-counter", self.boot_counter_path,
                    "--ready-delay", str(self.dry_run_ready_s),
                    "--event-plan", json.dumps(self.dry_run_event_plan),
                    "--cut-ms", str(self.dry_run_cut_ms),
                    "--unflushed-window-ms", str(self.dry_run_unflushed_ms)]
        argv = [self.qemu_binary(), "-machine", "q35", "-accel", "tcg",
                "-m", str(self.mem_mb), "-smp", str(self.cpus),
                "-display", "none", "-no-reboot", "-net", "none",
                "-device", "virtio-blk-pci,drive=ambre",
                "-drive", self.drive_spec(),
                "-serial", "unix:%s,server=on,wait=off" % self.serial_path,
                "-qmp", "unix:%s,server=on,wait=off" % self.qmp_path,
                "-pidfile", os.path.join(self.workdir, "qemu.pid")]
        return argv + self.extra_args

    def fake_vm_path(self):
        """Write (once) and return the path of the fake-VM script."""
        os.makedirs(self.workdir, exist_ok=True)
        path = os.path.join(self.workdir, "fake-vm.py")
        if not os.path.exists(path):
            with open(path, "w") as handle:
                handle.write(FAKE_VM_SCRIPT)
        return path

    # -- lifecycle ---------------------------------------------------------

    def start(self, timeout_s=None):
        """Start the machine and wait until it answers; returns boot seconds."""
        timeout_s = self.boot_timeout_s if timeout_s is None else timeout_s
        self._clean_stale_sockets()
        argv = self.command_line()
        started = time.monotonic()
        try:
            self.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                                         stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
        except OSError as exc:
            raise PowercutError("cannot start %s: %s" % (argv[0], exc))
        if not self.probe_ready(timeout_s):
            self.stop()
            raise PowercutError("guest not ready after %.1fs (%s)"
                                % (timeout_s, argv[0]))
        self.boot_seconds = time.monotonic() - started
        return self.boot_seconds

    def probe_ready(self, timeout_s=None):
        """Bounded readiness probe; True when the guest answers."""
        timeout_s = self.boot_timeout_s if timeout_s is None else timeout_s
        probe = self.dry_run_ready if self.backend == "dry-run" else self.console_answers
        try:
            wait_for(probe, timeout_s, 0.01, "guest readiness")
            return True
        except TimeoutError:
            return False

    def console_answers(self):
        """True when a trivial command succeeds on the serial console."""
        return self.run_guest("echo AMBRE_PROBE", timeout_s=5.0).ok

    def dry_run_ready(self):
        """True when the fake guest published its ready state."""
        if self.proc is None or self.proc.poll() is not None:
            return False
        return bool(self._read_json(self.state_path).get("ready"))

    @staticmethod
    def _read_json(path):
        try:
            with open(path) as handle:
                return json.load(handle)
        except (OSError, ValueError):
            return {}

    def run_guest(self, command, timeout_s=30.0):
        """Execute ``command`` in the guest; returns a :class:`GuestResult`."""
        if self.backend == "dry-run":
            return self.dry_run_guest(command)
        if self.console is None:
            self.console = SerialConsole(self.serial_path, self.console_prompt)
        return self.console.run(command, timeout_s)

    def dry_run_guest(self, command):
        """Simulated guest command: serves the fake guest journal."""
        if not self.dry_run_ready():
            return GuestResult(1, "", "guest not ready")
        return GuestResult(0, json.dumps({"events": self.dry_run_events()}), "")

    def dry_run_events(self):
        """Read the simulated guest journal (empty when nothing was durable)."""
        return parse_events(json.dumps(self._read_json(self.journal_path)))

    def power_cut(self):
        """Simulate a power cut: SIGKILL the machine.  Returns kill seconds."""
        if self.proc is None:
            return 0.0
        started = time.monotonic()
        try:
            self.proc.kill()
            self.proc.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.cut_seconds = time.monotonic() - started
        if self.console is not None:
            self.console.close()
            self.console = None
        return self.cut_seconds

    def restart(self, timeout_s=None):
        """Restart the machine after a cut; returns boot seconds."""
        if self.proc is not None and self.proc.poll() is None:
            self.power_cut()
        return self.start(timeout_s)

    def cycle(self, timeout_s=None):
        """Full cycle: cut + restart + ready; returns the duration in seconds."""
        started = time.monotonic()
        self.power_cut()
        self.restart(timeout_s)
        return time.monotonic() - started

    def stop(self):
        """Stop the machine and release its resources (idempotent)."""
        if self.console is not None:
            self.console.close()  # the socket must not outlive the process
            self.console = None
        terminate_process(self.proc)
        self.proc = None
        if self in _LIVE_VMS:
            _LIVE_VMS.remove(self)
        if self._owns_workdir and not self.keep_workdir:
            shutil.rmtree(self.workdir, ignore_errors=True)

    def _clean_stale_sockets(self):
        """Drop sockets left behind by a previous, killed machine."""
        for path in (self.serial_path, self.qmp_path):
            try:
                os.unlink(path)
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop()
        return False


def _stop_live_vms():  # pragma: no cover - interpreter shutdown helper
    for vm in list(_LIVE_VMS):
        try:
            vm.stop()
        except Exception:
            pass


atexit.register(_stop_live_vms)  # no orphan process, even on crash
