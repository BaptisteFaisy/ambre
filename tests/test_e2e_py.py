#!/usr/bin/env python3
"""Level-2 test of the Ambre chain: real processes, real files, real SIGKILL.

Run: ``python3 tests/test_e2e_py.py``
Exit code = number of failures (0 = green), the project convention.

Five cuts, one per window of the specification (cahier 4.3), each followed by
a restart on the SAME disk file and by the invariants that apply:

    pre        cut before the trigger       -> nothing to keep, no event
    acquired   cut after the flush          -> the event is there, sealed by
    (numeric)  cut inside the impact         the recovery, "complete file"
    sealed     cut after the seal            follows the window (I7), and the
    recovery   cut during the recovery       class A data equals what was
                                             injected (I2, I4, I8).

It needs the five binaries: ``make all`` (or AMBRE_BINS=/path/to/build).
Whole run: a few tens of seconds, inside the 5-minute CI budget.
English only, Python 3 standard library only.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "host", "e2e"))

import e2e_cycle      # noqa: E402
import e2e_env        # noqa: E402

DURATION_MS = 6000
CRASH_AT_MS = 3000
SEED = 20261111


def find_bins() -> str:
    """The build directory holding the processes. Fails loudly when absent:
    a skipped test would look like a green one."""
    candidates = []
    for var in ("AMBRE_BINS", "AMBRE_BUILD_DIR"):
        value = os.environ.get(var)
        if value:
            candidates.append(value if os.path.isabs(value)
                              else os.path.join(ROOT, value))
    candidates += [os.path.join(ROOT, "build"), os.path.join(ROOT, "build-e2e")]
    for path in candidates:
        if path and os.access(os.path.join(path, "edr-journal"), os.X_OK):
            return path
    raise RuntimeError(
        "no built binaries (looked in %s): run 'make all' first"
        % ", ".join(candidates))


def generate_scenario(path, seed=SEED):
    """Write the scenario every cut of this test plays."""
    subprocess.run(
        [sys.executable, os.path.join("host", "scenarios", "generate.py"),
         "--seed", str(seed), "--duration-ms", str(DURATION_MS),
         "--crash-at-ms", str(CRASH_AT_MS), "--out", path],
        cwd=ROOT, check=True, capture_output=True, text=True)
    return path


class CutCase(unittest.TestCase):
    """One scenario shared by the five cuts; one temporary tree per test."""

    @classmethod
    def setUpClass(cls):
        cls.bins = find_bins()
        cls.hold = tempfile.mkdtemp(prefix="ambre-e2e-test-")
        cls.scenario = generate_scenario(os.path.join(cls.hold,
                                                      "scenario.txt"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.hold, ignore_errors=True)

    def cycle(self, cut_at, seed=0):
        started = time.monotonic()
        result = e2e_cycle.run_cycle(self.bins, self.scenario, cut_at,
                                     verify=True, seed=SEED + seed)
        self.assertLess(time.monotonic() - started, 60.0,
                        "the cycle overran the 60 s budget")
        self.assertIsNone(result.get("error"), result.get("error"))
        return result

    def assert_no_violation(self, result):
        self.assertEqual([], result["violations"], result.get("journal_log"))
        self.assertNotEqual(e2e_cycle.VERDICT_ERROR, result["verdict"])

    def assert_data_faithful(self, result):
        fidelity = result["checks"]["fidelity"]
        self.assertTrue(fidelity["ok"], fidelity)
        official = result["checks"]["official"]
        self.assertTrue(official["ok"], official)

    # -- the five windows --------------------------------------------------

    def test_cut_before_the_trigger(self):
        """Nothing was triggered: nothing may appear (I3)."""
        result = self.cycle("pre", 1)
        self.assert_no_violation(result)
        self.assertEqual("before_acquisition", result["window"])
        self.assertEqual([], result["events"])

    def test_cut_right_after_acquisition(self):
        """Acquired before the cut: the event survives, incomplete (I2, I7)."""
        result = self.cycle("acquired", 2)
        self.assert_no_violation(result)
        self.assertIsNotNone(result["timeline"]["acquired_at"])
        self.assertTrue(result["events"], "the acquired event disappeared")
        primary = e2e_cycle.primary_event(result["events"])
        self.assertEqual("sealed", primary["state"])
        if result["window"] == "acquired_open":
            self.assertFalse(primary["file_complete"])
        self.assert_data_faithful(result)

    def test_cut_inside_the_impact(self):
        """A numeric instant inside the impact: either window is legal, the
        state must stay consistent and the data faithful."""
        result = self.cycle(CRASH_AT_MS + 90, 3)
        self.assert_no_violation(result)
        if result["events"]:
            self.assert_data_faithful(result)

    def test_cut_after_the_seal(self):
        """Sealed before the cut: the event survives as complete (I7)."""
        result = self.cycle("sealed", 4)
        self.assert_no_violation(result)
        self.assertIsNotNone(result["timeline"]["sealed_at"])
        primary = e2e_cycle.primary_event(result["events"])
        self.assertIsNotNone(primary)
        self.assertEqual("sealed", primary["state"])
        self.assertTrue(primary["file_complete"])
        self.assert_data_faithful(result)

    def test_cut_during_the_recovery(self):
        """A second cut while the recovery runs: same final state (I8)."""
        result = self.cycle("recovery", 5)
        self.assert_no_violation(result)
        self.assertTrue(result["recovery_cut"], "no recovery cut happened")
        primary = e2e_cycle.primary_event(result["events"])
        self.assertIsNotNone(primary)
        self.assertTrue(primary["file_complete"])
        self.assert_data_faithful(result)


class BenchHygieneCase(unittest.TestCase):
    """The bench must fail loudly, and must never leave a process behind."""

    def test_missing_binary_is_reported(self):
        with self.assertRaises(e2e_env.BenchError) as caught:
            e2e_env.resolve_binaries(os.path.join(ROOT, "no-such-build-dir"))
        self.assertIn("make", str(caught.exception))

    def test_no_orphan_process(self):
        before = _ambre_processes()
        bins = find_bins()
        bench = e2e_env.Bench(bins)
        bench.start_journal()
        bench.cleanup()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and _ambre_processes() != before:
            time.sleep(0.05)
        self.assertEqual(before, _ambre_processes())

    def test_bench_killed_by_signal_takes_its_children_and_files(self):
        """SIGTERM on the bench: no orphan process, no temporary tree left."""
        before = _ambre_processes()
        hold = tempfile.mkdtemp(prefix="ambre-e2e-sigterm-")
        self.addCleanup(shutil.rmtree, hold, True)
        scenario = generate_scenario(os.path.join(hold, "scenario.txt"))
        cmd = [sys.executable, os.path.join(ROOT, "host", "e2e", "e2e_run.py"),
               "--bins", find_bins(), "--scenario", scenario,
               "--cut-at", "sealed"]
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        trees = _temp_trees()
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline and _temp_trees() == trees:
            time.sleep(0.1)
        spawned = _temp_trees() - trees
        self.assertTrue(spawned, "the bench created no working directory")
        proc.terminate()
        self.assertEqual(proc.wait(timeout=20) is not None, True)
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and (
                _ambre_processes() != before or _temp_trees() & spawned):
            time.sleep(0.1)
        self.assertEqual(before, _ambre_processes(), "orphan process left")
        self.assertFalse(_temp_trees() & spawned, "temporary tree left")


def _temp_trees():
    """The set of working directories the bench created under /tmp."""
    import glob
    return set(glob.glob(os.path.join(tempfile.gettempdir(), "ambre-e2e-*")))


def _ambre_processes():
    """The set of Ambre process ids alive on this machine (orphan watch)."""
    try:
        out = subprocess.run(["pgrep", "-f", "edr-journal|edr-capture|"
                              "edr-replay"], capture_output=True, text=True,
                             timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return set()
    return {int(line) for line in out.stdout.split() if line.isdigit()}


if __name__ == "__main__":
    program = unittest.main(verbosity=2, exit=False)
    result = program.result
    failures = (len(result.failures) + len(result.errors)
                + len(result.unexpectedSuccesses))
    sys.exit(failures)
