"""Shared helpers for the Ambre power-cut level (spec 7.4).

Process control, bounded waits, guest result parsing and the serial console
live here so that qemu_vm.py, powercut_campaign.py, e1_flush_probe.py and
e3_cycle_timing.py never duplicate them.
"""

from __future__ import annotations

import json
import os
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


# -- shared helpers (used by every tool of the power-cut level) -------------

class GuestResult:
    """Outcome of a command executed inside the guest.

    Vit le ici et pas dans ``qemu_vm`` : ``run_capture()`` et ``SerialConsole``
    le construisent déjà, et le chemin d'erreur (pas de prompt, timeout, socket
    absent) levait un ``NameError`` au lieu de rendre le résultat — donc un banc
    muet se transformait en plantage, sans rapport. ``qemu_vm`` l'importe.
    """

    def __init__(self, returncode, stdout="", stderr=""):
        self.returncode, self.stdout, self.stderr = int(returncode), stdout or "", stderr or ""
        self.ok = self.returncode == 0  # exit status only


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
    for action in (proc.terminate, proc.kill):
        try:
            action()
            proc.wait(timeout=timeout_s)
            return True
        except (OSError, subprocess.TimeoutExpired):
            continue
    return proc.poll() is not None


def parse_events(payload):
    """Parse a guest export payload into a list of event dicts (tolerant)."""
    try:
        data = json.loads((payload or "").strip())
    except ValueError:
        return []
    if isinstance(data, dict):
        items = [data]
        for key in ("events", "records", "items", "data"):
            if isinstance(data.get(key), list):
                items = data[key]
                break
    elif isinstance(data, list):
        items = data
    else:
        return []
    return [item for item in items if isinstance(item, dict)]


def stats(values):
    """min / median / p99 / max / mean of a sample list ({"count": 0} if empty)."""
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
                     % (start, start + width,
                        "#" * int(round(40.0 * count / peak)), count))
    return "\n".join(lines) + "\n"


# -- guest transport: serial console on a unix socket (real QEMU) ----------

class SerialConsole:
    """Send a command on the guest serial line, read back to the prompt."""

    def __init__(self, sock_path, prompt=DEFAULT_PROMPT, strict_prompt=False):
        """``strict_prompt`` : la fin de lecture est décidée sur la DERNIÈRE ligne
        non vide, pas sur la fin du flux. À activer quand la commande envoyée se
        termine elle-même par le marqueur (le cas d'un shell de banc sans prompt,
        PS1 vidé) — sinon l'écho de la commande clôt la lecture avant la sortie.
        Défaut désactivé : les campagnes existantes ont un vrai prompt
        (`root@vm:/tmp#`), dont la ligne ne serait plus jamais égale à `#`."""
        self.sock_path, self.prompt, self.sock = sock_path, prompt, None
        self.strict_prompt = strict_prompt

    def connect(self, timeout_s=10.0):
        """Connect to the console socket, retrying within ``timeout_s``."""
        def attempt():
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(self.sock_path)
                self.sock = sock
                return True
            except OSError:
                sock.close()
                return False
        try:
            return wait_for(attempt, timeout_s, 0.05, "serial console socket")
        except TimeoutError:
            return False

    def run(self, command, timeout_s=30.0):
        """Send one command line; fails when the prompt never comes back."""
        if self.sock is None and not self.connect(min(timeout_s, 10.0)):
            return GuestResult(1, "", "serial console not available")
        try:
            self.sock.sendall((command + "\r\n").encode("utf-8", "replace"))
        except OSError as exc:
            return GuestResult(1, "", str(exc))
        text, seen_prompt = self._read_until_prompt(timeout_s)
        if not seen_prompt:
            return GuestResult(1, text, "no prompt within %.1fs" % timeout_s)
        return GuestResult(0, text, "")

    def _read_until_prompt(self, timeout_s):
        """Read until the prompt marker appears; returns (text, seen_prompt).

        La comparaison se fait sur la DERNIÈRE ligne non vide, pas sur la fin du
        flux : `endswith(prompt)` rendait la lecture dès l'ECHO de la commande
        tapée, quand cette commande se termine par le marqueur. Sur un shell sans
        prompt (PS1 vidé, le cas d'un banc QNX amorcé par `on -d -t`), le
        marqueur est alors lu avant toute sortie réelle — le banc rend un rc
        plausible et une sortie vide, ce qui est le pire genre d'échec.
        """
        deadline, chunks = time.monotonic() + timeout_s, []
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
            text = "".join(chunks)
            done = (self._at_prompt(text) if self.strict_prompt
                    else text.rstrip().endswith(self.prompt))
            if done:
                return text, True
        return "".join(chunks), False

    def _at_prompt(self, text):
        """True quand la dernière ligne non vide EST le prompt (pas l'écho)."""
        lines = [ln.strip() for ln in text.replace("\r\n", "\n").split("\n")]
        for line in reversed(lines):
            if line:
                return line == self.prompt
        return False

    def close(self):
        """Close the console socket (idempotent)."""
        try:
            if self.sock is not None:
                self.sock.close()
        except OSError:
            pass
        self.sock = None

