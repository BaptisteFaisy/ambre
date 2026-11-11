#!/usr/bin/env python3
"""EXP-01 / OBS-02 live on PC: the real edrctl talks to the real edr-journal.

Run: ``python3 tests/test_ctl_live_py.py``  (exit code = failures, 0 = green).

Why this suite exists (close-out, week 5): the frozen port header now defines
``AMBRE_PORT_HAS_CONN_REQUEST`` and declares ``ambre_port_conn_request()``,
the POSIX port implements it (``target/port/ambre_port_posix_ipc.c``), and
``target/ctl/edr_ctl.c`` compiles its real request/reply binding under that
macro. The old claim "the POSIX port has no client-side reply, edrctl cannot
complete an exchange on PC" is therefore STALE; this suite pins the working
behaviour end to end over a real AF_UNIX socket:

  * ``edrctl status`` (text) answers rc=0 with the OBS-02 fields;
  * ``edrctl status --json`` returns the FULL status body as one JSON object
    (the historical gap truncated replies at 32 bytes; the body is larger);
  * ``edrctl list`` answers rc=0 on an empty journal;
  * a misused command exits 2 (frozen CLI contract, docs/W2-PLAN.md);
  * a failed export exits 1 with an explicit NAK (no invented state);
  * ROB-02 (ctl half): SIGKILL on an edrctl client mid-exchange leaves the
    journal serving normally.

No QNX dependency. Needs the binaries: ``make all`` (or AMBRE_BINS=<dir>).
English only, Python 3 standard library only.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from ambre_py_bins import find_bins  # noqa: E402

READY_TIMEOUT_S = 15.0
STATUS_JSON_KEYS = (
    "mode", "backend", "health", "supervised", "current_seg", "current_block",
    "open_events", "acquired_events", "sealed_events", "refused_no_space",
    "bytes_written", "flush_count", "uptime_ms", "last_freeze_us",
    "worst_freeze_us",
)


class CtlLiveCase(unittest.TestCase):
    """One journal process shared by the whole suite."""

    @classmethod
    def setUpClass(cls):
        cls.bins = find_bins()
        cls.hold = tempfile.mkdtemp(prefix="ambre-ctl-live-")
        cls.sock = os.path.join(cls.hold, "journal.sock")
        cls.disk = os.path.join(cls.hold, "journal.img")
        cls.log = open(os.path.join(cls.hold, "journal.log"), "wb")
        cls.journal = subprocess.Popen(
            [os.path.join(cls.bins, "edr-journal"),
             "--disk", cls.disk, "--listen", cls.sock],
            stdout=cls.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
            env=dict(os.environ, AMBRE_LOG="1"))
        limit = time.monotonic() + READY_TIMEOUT_S
        while not os.path.exists(cls.sock):
            if time.monotonic() > limit:
                raise RuntimeError("edr-journal did not open %s\n%s"
                                   % (cls.sock, cls.journal_log()))
            if cls.journal.poll() is not None:
                raise RuntimeError("edr-journal died at start-up:\n%s"
                                   % cls.journal_log())
            time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "journal", None) and cls.journal.poll() is None:
            cls.journal.terminate()
            try:
                cls.journal.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.journal.kill()
                cls.journal.wait(timeout=5)
        if getattr(cls, "log", None):
            cls.log.close()
        shutil.rmtree(cls.hold, ignore_errors=True)

    @classmethod
    def journal_log(cls, lines=15):
        try:
            with open(os.path.join(cls.hold, "journal.log"), errors="replace") \
                    as handle:
                return "\n".join(handle.read().splitlines()[-lines:])
        except OSError:
            return "(no log)"

    def ctl(self, *args, timeout=10.0):
        """Run edrctl against the live journal; return (rc, stdout, stderr)."""
        proc = subprocess.run(
            [os.path.join(self.bins, "edrctl"),
             "--journal", self.sock, *args],
            capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr

    # -- EXP-01 / OBS-02: the exchange completes on the PC backend ---------

    def test_status_text_answers_with_the_service_state(self):
        rc, out, err = self.ctl("status")
        self.assertEqual(0, rc, err or self.journal_log())
        for field in ("mode", "health", "events", "bytes written"):
            self.assertIn(field, out)

    def test_status_json_carries_the_full_reply_body(self):
        """The whole STATUS_REPLY arrives: the 32-byte truncation gap that
        made the PC client path 'unsupported' is closed."""
        rc, out, err = self.ctl("status", "--json")
        self.assertEqual(0, rc, err or self.journal_log())
        body = json.loads(out.strip().splitlines()[-1])
        for key in STATUS_JSON_KEYS:
            self.assertIn(key, body)
        self.assertEqual("posix", body["backend"])
        self.assertEqual("ok", body["health"])
        self.assertGreater(len(out.strip()), 32,
                           "reply must exceed the historical 32-byte limit")

    def test_list_answers_on_an_empty_journal(self):
        rc, out, err = self.ctl("list")
        self.assertEqual(0, rc, err or self.journal_log())
        self.assertTrue(out.strip() != "", "list must say something")

    def test_usage_error_exits_2(self):
        """Frozen CLI contract (docs/W2-PLAN.md): 0 ok, 1 functional, 2 usage."""
        rc, _out, _err = self.ctl("nosuchcommand")
        self.assertEqual(2, rc)

    def test_failed_export_exits_1_with_an_explicit_nak(self):
        """No invented state: a refused export says so and fails."""
        missing = os.path.join(self.hold, "event-99.mcap")
        rc, out, err = self.ctl("export", "99", "--out", missing)
        self.assertEqual(1, rc)
        self.assertIn("NAK", out + err)
        self.assertFalse(os.path.exists(missing))

    # -- ROB-02 (ctl half): a killed client never disturbs the service -----

    def test_journal_survives_an_edrctl_killed_mid_exchange(self):
        victim = subprocess.Popen(
            [os.path.join(self.bins, "edrctl"),
             "--journal", self.sock, "status", "--json"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.kill(victim.pid, signal.SIGKILL)   # no grace: brutal client death */
        victim.wait(timeout=5)
        time.sleep(0.2)
        self.assertEqual(None, self.journal.poll(),
                         "the journal died after the client kill:\n%s"
                         % self.journal_log())
        rc, out, err = self.ctl("status")
        self.assertEqual(0, rc, "service stopped answering: %s" % (err,))
        self.assertIn("health", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
