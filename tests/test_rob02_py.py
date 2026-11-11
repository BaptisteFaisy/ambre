#!/usr/bin/env python3
"""ROB-02 runtime bench on PC: a crashed edr-archive never disturbs capture
nor the freeze.

Run: ``python3 tests/test_rob02_py.py``  (exit code = failures, 0 = green).

This is the runtime kill bench that docs/VALIDATION.md ROB-02 used to defer
as ``TODO(campaign)``. It needs no QNX: real processes, real AF_UNIX sockets,
a real ``SIGKILL`` on ``edr-archive`` while an event is being frozen.

Phases (one scenario, ~12 s wall clock):
  1. start the chain edr-replay -> edr-capture -> edr-journal (host/e2e
     bench) plus edr-archive watching the journal socket;
  2. wait until the journal reports the triggered event (acquired/sealed);
  3. SIGKILL edr-archive — no handler, no grace;
  4. assert the journal and the capture are untouched: the event still
     seals, a freeze latency was recorded, and class-B bytes keep flowing
     (the capture still records while the archive is dead);
  5. assert the service path still works without the archive: edrctl
     status/list answer and ``edrctl export`` produces the event container.

The edrctl half of ROB-02 (a client killed mid-exchange) is pinned by
``tests/test_ctl_live_py.py``. Needs the binaries: ``make all``
(or AMBRE_BINS=<dir>). English only, Python 3 standard library only.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "host", "e2e"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import e2e_env                      # noqa: E402
from ambre_py_bins import find_bins  # noqa: E402

DURATION_MS = 9000        # long enough to keep replaying after the kill
CRASH_AT_MS = 3000
SEED = 20261112
EVENT_TIMEOUT_S = 25.0


class Rob02Case(unittest.TestCase):
    """One live chain, one brutal archive death, five invariants."""

    def setUp(self):
        self.bins = find_bins(required=("edr-journal", "edr-capture",
                                        "edr-replay", "edr-archive", "edrctl"))
        self.hold = tempfile.mkdtemp(prefix="ambre-rob02-")
        self.scenario = os.path.join(self.hold, "scenario.txt")
        subprocess.run(
            [sys.executable, os.path.join("host", "scenarios", "generate.py"),
             "--seed", str(SEED), "--duration-ms", str(DURATION_MS),
             "--crash-at-ms", str(CRASH_AT_MS), "--out", self.scenario],
            cwd=ROOT, check=True, capture_output=True, text=True)
        self.bench = e2e_env.Bench(self.bins,
                                   workdir=os.path.join(self.hold, "run"))
        self.archive = None

    def tearDown(self):
        if self.archive is not None:
            e2e_env.stop_process(self.archive)
        self.bench.cleanup()
        shutil.rmtree(self.hold, ignore_errors=True)

    # -- helpers -----------------------------------------------------------

    def ctl_json(self, command):
        """One edrctl call; return its last JSON line (None when unusable)."""
        proc = subprocess.run(
            [os.path.join(self.bins, "edrctl"),
             "--journal", self.bench.journal_sock, command, "--json"],
            capture_output=True, text=True, timeout=10)
        if proc.returncode != 0 or not proc.stdout.strip():
            return None
        return json.loads(proc.stdout.strip().splitlines()[-1])

    def wait_events(self, predicate, what):
        """Poll edrctl until predicate(status_json) holds; fail on timeout."""
        limit = time.monotonic() + EVENT_TIMEOUT_S
        last = None
        while time.monotonic() < limit:
            last = self.ctl_json("status")
            if last is not None and predicate(last):
                return last
            time.sleep(0.05)
        self.fail("timeout waiting for %s (last status: %s)\njournal: %s"
                  % (what, last, self.bench.tail("journal.log")))

    # -- the bench ----------------------------------------------------------

    def _start_chain(self):
        """journal + capture + archive + replay; return the archive out dir."""
        self.bench.start_journal()
        self.bench.start_capture()
        outdir = os.path.join(self.hold, "archive-out")
        os.makedirs(outdir, exist_ok=True)
        self.archive = e2e_env.spawn(
            [os.path.join(self.bins, "edr-archive"),
             "--journal", self.bench.journal_sock, "--out", outdir],
            os.path.join(self.hold, "archive.log"),
            dict(os.environ, AMBRE_LOG="1"))
        self.bench.start_replay(self.scenario, rate=1.0)

    def _kill_archive_at_first_event(self):
        """Wait for the triggered event, SIGKILL the archive right there."""
        status = self.wait_events(
            lambda s: s["acquired_events"] + s["sealed_events"] >= 1,
            "the triggered event")
        killed_before_seal = status["sealed_events"] == 0
        self.archive.kill()
        self.archive.wait(timeout=5)
        return status, killed_before_seal

    def _assert_freeze_completes(self):
        """The journal survives and seals the event anyway (GEL/REP path)."""
        self.assertEqual(None, self.bench.journal.poll(),
                         "the journal died with the archive:\n%s"
                         % self.bench.tail("journal.log"))
        status = self.wait_events(lambda s: s["sealed_events"] >= 1,
                                  "the sealed event")
        self.assertGreater(status["worst_freeze_us"], 0,
                           "no freeze latency recorded (GEL-02 path)")
        return status

    def _assert_capture_still_feeds(self, status, killed_before_seal):
        """Class-B bytes keep flowing while the archive is dead."""
        written_before = status["bytes_written"]
        time.sleep(1.0)
        status2 = self.ctl_json("status")
        self.assertIsNotNone(status2, "the journal stopped answering edrctl")
        self.assertGreater(status2["bytes_written"], written_before,
                           "capture stopped feeding the journal after the "
                           "archive kill (killed before seal: %s)"
                           % killed_before_seal)

    def _assert_service_path_works(self):
        """edrctl list/export still work with the archive dead."""
        events = subprocess.run(
            [os.path.join(self.bins, "edrctl"),
             "--journal", self.bench.journal_sock, "list", "--json"],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(0, events.returncode, events.stderr)
        line = json.loads(events.stdout.strip().splitlines()[0])
        self.assertEqual("sealed", line["state"])
        container = os.path.join(self.hold, "event.mcap")
        export = subprocess.run(
            [os.path.join(self.bins, "edrctl"),
             "--journal", self.bench.journal_sock,
             "export", str(line["event_id"]), "--out", container],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(0, export.returncode,
                         export.stdout + export.stderr +
                         self.bench.tail("journal.log"))
        self.assertGreater(os.path.getsize(container), 0)

    def test_archive_sigkill_does_not_disturb_capture_nor_freeze(self):
        self._start_chain()
        status, killed_before_seal = self._kill_archive_at_first_event()
        status = self._assert_freeze_completes()
        self._assert_capture_still_feeds(status, killed_before_seal)
        self._assert_service_path_works()


if __name__ == "__main__":
    unittest.main(verbosity=2)
