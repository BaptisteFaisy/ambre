#!/usr/bin/env python3
"""Autonomous tests of the latency campaign harness (host/latency/).

Run with:  python3 tests/test_latency_py.py
Exit code: number of failing tests (0 = green).

The integration test needs the three PC binaries (edr-journal, edr-capture,
edr-replay); it looks for them in $AMBRE_BINS, then build-lat/, then build/,
and skips with a message when none is present.  Total runtime stays well
under 90 s: the campaign runs in --quick mode on a two-shock scenario.
"""

import json
import os
import shutil
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "host", "latency"))
sys.path.insert(0, os.path.join(ROOT, "host", "mcap"))

import campaign                                        # noqa: E402
import chain as chain_mod                              # noqa: E402
import latency_stats as stats                         # noqa: E402
import load_gen                                        # noqa: E402
import measure                                         # noqa: E402
import multi_shock                                     # noqa: E402

NEEDED_BINS = ("edr-journal", "edr-capture", "edr-replay")


def find_bins():
    """First build directory holding the three PC binaries, else None."""
    candidates = []
    if os.environ.get("AMBRE_BINS"):
        candidates.append(os.environ["AMBRE_BINS"])
    candidates += [os.path.join(ROOT, name)
                   for name in ("build-lat", "build", "build-ci", "build-final")]
    for directory in candidates:
        if all(os.access(os.path.join(directory, b), os.X_OK)
               for b in NEEDED_BINS):
            return directory
    return None


BINS = find_bins()


# ------------------------------------------------------------- statistics --

class TestStatistics(unittest.TestCase):
    def test_nearest_rank_percentiles(self):
        values = list(range(1, 101))            # 1..100
        self.assertEqual(stats.percentile(values, 50), 50)
        self.assertEqual(stats.percentile(values, 99), 99)
        self.assertEqual(stats.percentile(values, 99.9), 100)
        self.assertEqual(stats.percentile(values, 100), 100)

    def test_percentile_unsorted_and_odd(self):
        values = [9, 1, 5, 3, 7]
        self.assertEqual(stats.percentile(values, 50), 5)
        self.assertEqual(stats.percentile(values, 99), 9)
        self.assertEqual(stats.summarize(values)["max"], 9)
        self.assertEqual(stats.summarize(values)["median"], 5)

    def test_summarize_known_set(self):
        summary = stats.summarize([10, 20, 30, 40, 50])
        self.assertEqual(summary["count"], 5)
        self.assertEqual(summary["min"], 10)
        self.assertEqual(summary["median"], 30)
        self.assertEqual(summary["max"], 50)
        self.assertEqual(summary["mean"], 30.0)
        self.assertEqual(summary["p99"], 50)

    def test_summarize_empty(self):
        self.assertEqual(stats.summarize([]), {"count": 0})
        self.assertIsNone(stats.percentile([], 99))

    def test_histogram_counts(self):
        counts = stats.histogram([10, 20, 300, 5000], (50, 100, 1000))
        self.assertEqual(counts, [2, 0, 1, 1])

    def test_render_histogram_lines(self):
        text = stats.render_histogram([10, 300], (50, 100), unit="us",
                                      title="t")
        self.assertIn("t", text)
        self.assertIn("50.0%", text)


# ------------------------------------------------------------- scenarios ---

class TestScenarioSplit(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ambre-lat-test-")
        self.path = os.path.join(self.tmp, "sc.txt")
        self.assertEqual(multi_shock.main(
            ["--triggers", "3", "--cycle-ms", "400", "--lead-ms", "200",
             "--tail-ms", "200", "--out", self.path]), 0)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_generated_scenario_has_three_shocks(self):
        header, rows = stats.parse_scenario(self.path)
        self.assertEqual(header["rate_hz"], "1000")
        self.assertEqual(len(rows), 3 * 400 + 200 + 200)
        self.assertEqual(len(stats.find_shocks(rows)), 3)

    def test_chunk_is_regular_and_rebased(self):
        _header, rows = stats.parse_scenario(self.path)
        chunk = stats.build_chunk(rows, stats.find_shocks(rows)[0])
        self.assertEqual(chunk[0][0], 0)
        for index, row in enumerate(chunk):
            self.assertEqual(row[0], index * 1000)
        hot = [row for row in chunk if abs(row[1]) >= 2000]
        self.assertTrue(hot)
        # a 10-row frame must stay under the 10 ms window of ACQ-01
        for start in range(0, len(chunk) - 9, 10):
            span = chunk[start + 9][0] - chunk[start][0]
            self.assertLess(span, 10000)

    def test_chunk_roundtrip_through_the_loader_format(self):
        _header, rows = stats.parse_scenario(self.path)
        chunk = stats.build_chunk(rows, stats.find_shocks(rows)[1])
        out = os.path.join(self.tmp, "chunk.txt")
        stats.write_scenario(out, chunk)
        header2, rows2 = stats.parse_scenario(out)
        self.assertEqual(header2["rate_hz"], "1000")
        self.assertEqual(rows2, chunk)

    def test_pulse_stays_unlocked(self):
        for dv in (9.0, 10.8, 12.6, 24.0):
            length = multi_shock.pulse_length_ms(dv)
            self.assertGreaterEqual(length * 250, int(dv * 1000 / 3.6))
            self.assertLess(length * 250, 6944)     # AMBRE_DV_LOCK_MM_S

    def test_dv_out_of_range_is_refused(self):
        with self.assertRaises(SystemExit):
            multi_shock.main(["--triggers", "1", "--dv-kmh", "30",
                              "--out", os.path.join(self.tmp, "x.txt")])


# ---------------------------------------------------------------- traces ---

class TestTraceParsing(unittest.TestCase):
    def test_parse_trace_and_replay_histogram(self):
        tmp = tempfile.mkdtemp(prefix="ambre-lat-test-")
        try:
            path = os.path.join(tmp, "trace.txt")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("100 1 1\n102 1 2\n110 1 3\n120 1 4\n"
                             "200 2 1\n202 2 2\n")
            trace = chain_mod.parse_trace(path)
            self.assertEqual(trace[1][stats.TRACE_FLUSH_ACK], 110)
            self.assertNotIn(3, trace[2])
            self.assertEqual(chain_mod.parse_trace(path + ".missing"), {})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_event_metrics_skips_refused_triggers(self):
        trace = {1: {1: 100, 2: 102, 3: 110, 4: 120}, 2: {1: 200, 2: 202}}
        rows = campaign.event_metrics(trace, {1: {"trig_ts_us": 90}}, run=7)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["perf01_us"], 10)
        self.assertEqual(rows[0]["freeze_us"], 10)
        self.assertEqual(rows[0]["dispatch_us"], 2)
        self.assertEqual(rows[0]["write_flush_us"], 8)
        self.assertEqual(rows[0]["seal_after_us"], 10)
        self.assertTrue(rows[0]["is_first_of_crash"])
        self.assertEqual(rows[0]["run"], 7)

    def test_replay_histogram_parsing(self):
        text = ("edr-replay: 20 frames, 400 samples, 0 external signal(s) "
                "in 200 ms\n"
                "send round trip (PERF-05 / essay E4): 20 messages, "
                "mean 1200 ns, worst 9000 ns\n"
                "  p50 <= 1000 ns, p99 <= 5000 ns, p99.9 <= 9000 ns\n"
                "  <=   1000 ns :       10  (50.00%)\n"
                "  <=   5000 ns :        9  (45.00%)\n"
                "  >    5000 ns :        1  ( 5.00%)\n"
                "  verdict: p99 meets the 50 us requirement\n")
        parsed = chain_mod.parse_replay_output(text)
        self.assertEqual(parsed["messages"], 20)
        self.assertEqual(parsed["mean_ns"], 1200)
        self.assertEqual(parsed["worst_ns"], 9000)
        self.assertEqual(parsed["p99_ns"], 5000)
        self.assertEqual(parsed["buckets"], [[1000, 10], [5000, 9]])
        self.assertEqual(parsed["overflow"], [5000, 1])
        self.assertEqual(parsed["verdict"], "meets")

    def test_pooled_percentile(self):
        pooled = campaign.pool_histograms(
            [{"messages": 10, "buckets": [[1000, 9], [5000, 1]]},
             {"messages": 10, "buckets": [[1000, 8], [5000, 2]]}])
        self.assertEqual(pooled["messages"], 20)
        self.assertEqual(campaign.percentile_from_buckets(
            pooled["buckets"], pooled["messages"], 99), 5000)
        self.assertEqual(campaign.percentile_from_buckets(
            pooled["buckets"], pooled["messages"], 50), 1000)
        self.assertIsNone(campaign.percentile_from_buckets({}, 0, 99))


# ------------------------------------------------------------ load agents --

class TestLoadGenerators(unittest.TestCase):
    def _cycle(self, kind, **kwargs):
        procs, description = load_gen.start_load(kind, 30, **kwargs)
        self.assertTrue(procs)
        self.assertEqual(description["kind"], kind)
        self.assertTrue(description["what"])
        time.sleep(0.6)
        alive = [p for p in procs if p.is_alive()]
        self.assertEqual(len(alive), len(procs))
        load_gen.stop_load(procs)
        self.assertFalse(any(p.is_alive() for p in procs))

    def test_cpu_load_starts_and_stops(self):
        self._cycle("cpu", workers=2)

    def test_disk_load_starts_and_stops_and_cleans(self):
        tmp = tempfile.mkdtemp(prefix="ambre-lat-disk-")
        try:
            self._cycle("disk", directory=tmp)
            self.assertEqual(os.listdir(tmp), [])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_mem_load_starts_and_stops(self):
        self._cycle("mem", ratio=0.02)

    def test_cli_and_unknown_kind(self):
        self.assertEqual(load_gen.main(
            ["--kind", "cpu", "--seconds", "1", "--workers", "1"]), 0)
        procs, description = load_gen.start_load("nothing", 1)
        self.assertEqual(procs, [])
        self.assertEqual(description["what"], "no load")

    def test_mem_available_is_read(self):
        self.assertGreater(load_gen.mem_available_bytes(), 0)


# ------------------------------------------------------- campaign (quick) --

@unittest.skipUnless(BINS, "no build directory with the three PC binaries")
class TestQuickCampaign(unittest.TestCase):
    def test_quick_report_is_complete_and_honest(self):
        tmp = tempfile.mkdtemp(prefix="ambre-lat-campaign-")
        try:
            scenario = os.path.join(tmp, "sc.txt")
            self.assertEqual(multi_shock.main(
                ["--triggers", "2", "--cycle-ms", "400", "--lead-ms", "200",
                 "--tail-ms", "200", "--out", scenario]), 0)
            report_path = os.path.join(tmp, "report.json")
            rc = measure.main([
                "--bins", BINS, "--scenario", scenario, "--triggers", "2",
                "--load", "none", "--quick", "--report", report_path,
                "--workdir", os.path.join(tmp, "work")])
            self.assertEqual(rc, 0)
            with open(report_path, encoding="utf-8") as handle:
                report = json.load(handle)
            self.check_report(report)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def check_report(self, report):
        self.assertEqual(report["backend"], "posix")
        self.assertTrue(report["kernel"])
        self.assertTrue(report["compiler"])
        for key in ("cpu", "cores", "ram_total_bytes"):
            self.assertIn(key, report["host"])
        missing = report["not_measurable_without_qnx"]
        self.assertIsInstance(missing, list)
        self.assertTrue(missing)
        self.assertTrue(any("PERF-06" in item for item in missing))
        self.assertTrue(any("PERF-07" in item for item in missing))
        self.assertTrue(any("PERF-08" in item for item in missing))
        for name in ("media", "software"):
            self.assertIn(name, report["passes"])
        for key in ("PERF-01", "PERF-02", "PERF-03", "PERF-05"):
            self.assertIn(key, report["requirements"])
        events = report["events"]["media"]
        self.assertTrue(events, "the quick campaign measured no event")
        for event in events:
            self.assertIsNotNone(event["freeze_us"])
            self.assertGreaterEqual(event["freeze_us"], 0)
        self.assertTrue(report["worst_cases"])
        self.assertIn("freeze_software_median_us", report["decomposition_us"])


def main():
    suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return len(result.failures) + len(result.errors)


if __name__ == "__main__":
    sys.exit(main())
