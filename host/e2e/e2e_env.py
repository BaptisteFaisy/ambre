#!/usr/bin/env python3
"""Process bench for the Ambre level-2 test: real processes, real files,
real SIGKILL.

What this module owns (and what it guarantees):

* every child process is started in its own process group, is killed on
  exit (SIGTERM, then SIGKILL), and is never left behind;
* every socket and every file lives in a temporary directory that is
  removed at the end (unless the caller asks to keep it);
* the journal is killed with SIGKILL, which is a real brutal death: no
  handler, no flush, no goodbye.

English only, Python 3 standard library only.
"""

from __future__ import annotations

import atexit
import os
import shutil
import signal
import subprocess
import tempfile
import time

import ambre_wire as wire

# Every child started here is registered here, so that an exception in the
# middle of a cycle can never leave an orphan behind. The same goes for the
# temporary trees the bench creates itself.
_LIVE = []
_LIVE_DIRS = []


def _forget(proc) -> None:
    if proc in _LIVE:
        _LIVE.remove(proc)


def _remove_dirs() -> None:
    """Remove the temporary trees the bench created (never a user directory)."""
    for path in list(_LIVE_DIRS):
        shutil.rmtree(path, ignore_errors=True)
        _LIVE_DIRS.remove(path)


@atexit.register
def _kill_everything() -> None:
    """Last resort: no process and no temporary file of this bench survives."""
    for proc in list(_LIVE):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        _forget(proc)
    _remove_dirs()


def _on_fatal_signal(signum, _frame) -> None:
    """A bench killed by a signal must still bury its children and its files."""
    _kill_everything()
    signal.signal(signum, signal.SIG_DFL)
    os.kill(os.getpid(), signum)


def install_signal_handlers() -> None:
    """Install the fatal-signal cleanup once (idempotent)."""
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        try:
            if signal.getsignal(sig) in (signal.SIG_DFL, None):
                signal.signal(sig, _on_fatal_signal)
        except (OSError, ValueError):
            pass


class BenchError(Exception):
    """The bench cannot run (missing binary, unusable scenario, ...)."""


def resolve_binaries(bins_dir: str) -> dict:
    """Locate the five binaries; raise BenchError with a clear message."""
    wanted = ("edr-journal", "edr-capture", "edr-replay")
    found = {}
    for name in wanted:
        path = os.path.join(bins_dir, name)
        if not os.access(path, os.X_OK):
            raise BenchError("missing binary: %s\n"
                             "Build it first: make BUILD=%s all" % (path, bins_dir))
        found[name] = path
    found["edrctl"] = wire.find_edrctl(bins_dir)
    return found


def spawn(cmd, log_path, env=None):
    """Start a child in its own process group, with its output in a file."""
    install_signal_handlers()
    log = open(log_path, "wb")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True,
                            env=env or os.environ.copy())
    proc.log_file = log
    proc.log_path = log_path
    _LIVE.append(proc)
    return proc


def stop_process(proc, grace_s: float = 2.0) -> int:
    """SIGTERM, then SIGKILL; returns the exit status (-1 if still alive)."""
    if proc is None or proc.poll() is not None:
        _forget(proc)
        return proc.poll() if proc else 0
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except (OSError, ProcessLookupError):
        pass
    try:
        proc.wait(timeout=grace_s)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        try:
            proc.wait(timeout=grace_s)
        except subprocess.TimeoutExpired:
            pass
    _forget(proc)
    close_log(proc)
    return proc.returncode if proc.returncode is not None else -1


def close_log(proc) -> None:
    handle = getattr(proc, "log_file", None)
    if handle is not None and not handle.closed:
        handle.close()


def kill_process(proc) -> float:
    """SIGKILL a process group and wait for it; returns the kill instant."""
    stamp = time.monotonic()
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (OSError, ProcessLookupError):
        pass
    try:
        proc.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        pass
    _forget(proc)
    close_log(proc)
    return stamp


class Bench:
    """One bench instance: a disk file, two sockets, three processes."""

    def __init__(self, bins_dir: str, workdir: str = None, keep: bool = False,
                 log_level: str = "1"):
        self.bins = resolve_binaries(bins_dir)
        self.keep = keep
        if workdir:
            os.makedirs(workdir, exist_ok=True)
        self.workdir = workdir or tempfile.mkdtemp(prefix="ambre-e2e-")
        if workdir is None and not keep:
            # Only a tree this bench created may be removed behind the
            # caller's back, including from a fatal-signal handler.
            _LIVE_DIRS.append(self.workdir)
        self.log_level = log_level
        self.disk = os.path.join(self.workdir, "journal.img")
        if workdir and os.path.exists(self.disk):
            # Refuse to start on somebody else's partition: the recovery would
            # read data this run never wrote, and the result would mean
            # nothing.
            raise BenchError("workdir %s already holds a journal image (%s); "
                             "remove it or choose another directory"
                             % (workdir, self.disk))
        self.journal_sock = os.path.join(self.workdir, "journal.sock")
        self.capture_sock = os.path.join(self.workdir, "capture.sock")
        self.journal = None
        self.capture = None
        self.replay = None
        self.env = dict(os.environ, AMBRE_LOG=log_level)

    # -- processes ---------------------------------------------------------

    def start_journal(self, extra_args=None, ready_timeout: float = 30.0):
        """Start edr-journal on the disk file and wait until it answers."""
        cmd = [self.bins["edr-journal"], "--disk", self.disk,
               "--listen", self.journal_sock, "--mode", "continuous"]
        cmd += list(extra_args or [])
        self.journal = spawn(cmd, os.path.join(self.workdir, "journal.log"),
                             self.env)
        try:
            link = wire.connect_when_ready(self.journal_sock, ready_timeout)
        except wire.WireError as exc:
            raise BenchError("edr-journal did not start within %.0f s: %s\n%s"
                             % (ready_timeout, exc,
                                self.tail("journal.log"))) from exc
        link.close()
        return self.journal

    def start_capture(self, ready_timeout: float = 10.0):
        """Start edr-capture and wait until its socket answers."""
        cmd = [self.bins["edr-capture"], "--journal", self.journal_sock,
               "--listen", self.capture_sock]
        self.capture = spawn(cmd, os.path.join(self.workdir, "capture.log"),
                             self.env)
        limit = time.monotonic() + ready_timeout
        while time.monotonic() < limit:
            if os.path.exists(self.capture_sock):
                return self.capture
            if self.capture.poll() is not None:
                raise BenchError("edr-capture died at start-up:\n%s"
                                 % self.tail("capture.log"))
            time.sleep(0.002)
        raise BenchError("edr-capture did not listen on %s" % self.capture_sock)

    def start_replay(self, scenario: str, rate: float = 1.0):
        """Start edr-replay against the capture socket."""
        cmd = [self.bins["edr-replay"], "--capture", self.capture_sock,
               "--scenario", scenario, "--rate", "%f" % rate]
        self.replay = spawn(cmd, os.path.join(self.workdir, "replay.log"),
                            self.env)
        return self.replay

    def cut_journal(self):
        """SIGKILL the journal: no handler, no flush (level-2 cut)."""
        if self.journal is None:
            return time.monotonic()
        return kill_process(self.journal)

    def stop_all(self) -> None:
        for proc in (self.replay, self.capture, self.journal):
            stop_process(proc)
        self.replay = self.capture = self.journal = None

    def cleanup(self) -> None:
        """Stop everything and remove the temporary tree."""
        self.stop_all()
        if not self.keep:
            shutil.rmtree(self.workdir, ignore_errors=True)
        if self.workdir in _LIVE_DIRS:
            _LIVE_DIRS.remove(self.workdir)

    # -- helpers -----------------------------------------------------------

    def tail(self, name: str, lines: int = 12) -> str:
        path = os.path.join(self.workdir, name)
        try:
            with open(path, "r", errors="replace") as handle:
                content = handle.read().splitlines()
        except OSError:
            return "(no log)"
        return "\n".join(content[-lines:])

    def log_text(self, name: str) -> str:
        path = os.path.join(self.workdir, name)
        try:
            with open(path, "r", errors="replace") as handle:
                return handle.read()
        except OSError:
            return ""
