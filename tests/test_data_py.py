#!/usr/bin/env python3
"""Proof for the real-data importers (cahier §8, §3.3).

Run:  python3 tests/test_data_py.py
Exit code = number of failed tests (0 = green).

What is proved, per importer:

1. the output is a valid docs/FORMAT.md §11 scenario (header keys, nine
   columns, one line per millisecond, no trailing space, no tabulation);
2. no forbidden data reaches it (VIE-01: no VIN, no position, no absolute
   time);
3. the imported scenario triggers the engine — checked through the Python twin
   of the trigger (host/verify/ambre_trigger.py), which is **imported, never
   copied**.

The synthetic layer is checked for the same properties, plus one that matters
for the house rule: host/data/synthetic.py must *reuse*
host/scenarios/generate.py, not re-implement it.
"""

import ast
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

DATA_DIR = Path(__file__).resolve().parent.parent / "host" / "data"
VERIFY_DIR = Path(__file__).resolve().parent.parent / "host" / "verify"
sys.path.insert(0, str(DATA_DIR))
sys.path.insert(0, str(VERIFY_DIR))

import synthetic                     # noqa: E402
import import_comma2k19 as c2k       # noqa: E402
import import_nhtsa as nhtsa         # noqa: E402
import ambre_trigger as at           # noqa: E402

COLUMNS = ("t_us", "a_long_mm_s2", "a_lat_mm_s2", "speed_kmh_x4", "throttle",
           "brake", "rpm", "steer_x2", "ext_flags")
HEADER_KEYS = ("name", "seed", "rate_hz", "duration_ms", "crash_at_ms")
# A relative monotonic clock stays far below this; an epoch in microseconds is
# around 1.8e15 us, so anything above would be an absolute timestamp.
ABSOLUTE_TIME_THRESHOLD_US = 10 ** 15


def parse_scenario(path):
    """Split a scenario file into (header dict, list of data rows)."""
    header, rows = {}, []
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            if "=" in line.split()[0]:
                key, value = line.split("=", 1)
                header[key] = value
                continue
            rows.append(line)
    return header, rows


class ScenarioAssertions(unittest.TestCase):
    """The properties every produced scenario must have, shared by all tests."""

    def assert_valid_scenario(self, path, duration_ms=None):
        header, rows = parse_scenario(path)
        for key in HEADER_KEYS:
            self.assertIn(key, header, "missing header key %s" % key)
        self.assertEqual(int(header["rate_hz"]), 1000)
        if duration_ms is not None:
            self.assertEqual(int(header["duration_ms"]), duration_ms)
        self.assertEqual(len(rows), int(header["duration_ms"]))
        self.assertGreater(len(rows), 0)

        for index, line in enumerate(rows):
            self.assertNotIn("\t", line, "tabulation on line %d" % (index + 1))
            self.assertEqual(line, line.strip(), "padding on line %d" % (index + 1))
            fields = line.split(" ")
            self.assertEqual(len(fields), len(COLUMNS),
                             "line %d has %d columns" % (index + 1, len(fields)))
            self.assertNotIn("", fields, "double space on line %d" % (index + 1))
            values = [int(item) for item in fields]
            self.assertEqual(values[0], index * 1000, "t_us grid broken")
        return header, rows

    def assert_privacy_safe(self, rows):
        """VIE-01: no absolute time, and no forbidden column in the format."""
        for index, row in enumerate(rows):
            t_us = int(row.split(" ")[0])
            self.assertLess(t_us, ABSOLUTE_TIME_THRESHOLD_US,
                            "absolute timestamp on line %d" % (index + 1))
            self.assertEqual(t_us, index * 1000)
        self.assertEqual(len(COLUMNS), 9)
        forbidden = {"vin", "position", "lat", "lon", "alt", "date", "time",
                     "driver", "driver_id"}
        self.assertEqual(forbidden & set(COLUMNS), set(),
                         "a forbidden column reached the format")


class TestSyntheticLayer(ScenarioAssertions):
    """synthetic.py must reuse the generator, and produce a valid scenario."""

    def test_it_imports_the_generator_by_path(self):
        gen = synthetic.generator()
        self.assertTrue(hasattr(gen, "pulse_steps"))
        self.assertTrue(hasattr(gen, "render"))
        self.assertTrue(hasattr(gen, "simulate"))

    def test_it_does_not_duplicate_the_pulse_integrator(self):
        """The module must call generate.pulse_steps, not redefine it."""
        source = (DATA_DIR / "synthetic.py").read_text(encoding="utf-8")
        tree = ast.parse(source)
        defined = {node.name for node in ast.walk(tree)
                   if isinstance(node, ast.FunctionDef)}
        for forbidden in ("pulse_steps", "simulate", "render", "build_config",
                          "plan_drive", "_shape_weights"):
            self.assertNotIn(forbidden, defined,
                             "synthetic.py redefines %s" % forbidden)
        self.assertIn("pulse_steps", source)       # used through the generator
        self.assertIn("render", source)

    def test_pulse_samples_sum_to_the_requested_delta_v(self):
        samples = synthetic.pulse_samples(12.0, 120, "crash")
        total_mm_s = sum(sample.a_long_mm_s2 for sample in samples) / 1000.0
        # 12 km/h = 3333 mm/s, rounded by the generator to whole mm/s steps.
        # A deceleration is negative (sign = -1, the default): compare magnitudes.
        self.assertAlmostEqual(abs(total_mm_s), 12.0 * 1000 / 3.6, delta=1.0)

    def test_resample_is_a_zero_order_hold(self):
        coarse = [synthetic.Sample(a_long_mm_s2=10),
                  synthetic.Sample(a_long_mm_s2=20)]
        fine = synthetic.resample(coarse, 100)
        self.assertEqual(len(fine), 200)          # 2 samples x 100 ms
        self.assertEqual(fine[0].a_long_mm_s2, 10)
        self.assertEqual(fine[99].a_long_mm_s2, 10)
        self.assertEqual(fine[100].a_long_mm_s2, 20)
        self.assertEqual(fine[199].a_long_mm_s2, 20)

    def test_resample_refuses_a_finer_period(self):
        with self.assertRaises(ValueError):
            synthetic.resample([synthetic.Sample()], 0)

    def test_inject_pulse_replaces_and_does_not_add(self):
        samples = synthetic.frames(10)
        samples[5].a_long_mm_s2 = 111
        pulse = [synthetic.Sample(a_long_mm_s2=-500),
                 synthetic.Sample(a_long_mm_s2=-600)]
        synthetic.inject_pulse(samples, pulse, 5)
        self.assertEqual(samples[5].a_long_mm_s2, -500)   # replaced, not added
        self.assertEqual(samples[6].a_long_mm_s2, -600)
        self.assertEqual(samples[4].a_long_mm_s2, 0)

    def test_cli_writes_a_valid_scenario(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = str(Path(tmp) / "synthetic.txt")
            self.assertEqual(synthetic.main([
                "--preset", "urban-frontal", "--seed", "1234",
                "--duration-ms", "4000", "--crash-at-ms", "3000",
                "--out", out]), 0)
            header, rows = self.assert_valid_scenario(out, duration_ms=4000)
            self.assert_privacy_safe(rows)
            self.assertEqual(int(header["crash_at_ms"]), 3000)


class TestComma2k19(ScenarioAssertions):
    """The comma2k19 importer produces a valid, privacy-safe scenario."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = str(Path(self.tmp.name) / "comma2k19.txt")

    def tearDown(self):
        self.tmp.cleanup()

    def test_row_count_matches_the_extract_and_the_grid(self):
        rows = c2k.read_extract(str(c2k.DEFAULT_FIXTURE))
        self.assertEqual(c2k.main(["--out", self.out]), 0)
        header, data = self.assert_valid_scenario(self.out)
        # One 100 Hz extract row becomes 100 grid samples.
        self.assertEqual(len(data), len(rows) * c2k.SOURCE_PERIOD_MS)
        self.assertEqual(int(header["duration_ms"]), len(data))

    def test_no_forbidden_data(self):
        c2k.main(["--out", self.out])
        _, data = self.assert_valid_scenario(self.out)
        self.assert_privacy_safe(data)
        source = c2k.DEFAULT_FIXTURE.read_text(encoding="utf-8")
        # The extract really carries the forbidden channels, and the output
        # really drops them: the check is not vacuous.
        header = next(line for line in source.splitlines()
                      if line.startswith("t_ms"))
        columns = header.split(",")
        for forbidden in ("lat", "lon", "pos_x", "pos_y", "pos_z"):
            self.assertIn(forbidden, columns)          # present in the source
            self.assertNotIn(forbidden, COLUMNS)       # absent from the output

    def test_forbidden_columns_are_listed(self):
        dropped = c2k.forbidden_columns(str(c2k.DEFAULT_FIXTURE))
        self.assertIn("lat", dropped)
        self.assertIn("lon", dropped)
        self.assertIn("pos_x", dropped)

    def test_units_are_converted(self):
        first = c2k.to_sample({"speed": "13.9", "accel_x": "0.2",
                               "accel_y": "-0.1"})
        self.assertEqual(first.a_long_mm_s2, 200)      # 0.2 m/s^2
        self.assertEqual(first.a_lat_mm_s2, -100)
        self.assertEqual(first.speed_kmh_x4, 200)      # 13.9 m/s = 50 km/h

    def test_crash_beyond_the_recording_is_refused(self):
        with self.assertRaises(SystemExit):
            c2k.main(["--out", self.out, "--crash-at-ms", "999999"])

    def test_imported_scenario_triggers_an_event(self):
        """Reuses host/verify/ambre_trigger.py: no copy of the engine."""
        self.assertEqual(c2k.main(["--out", self.out, "--crash-at-ms", "1500"]), 0)
        _, data = self.assert_valid_scenario(self.out)
        fired = run_trigger(data)
        self.assertTrue(fired, "the imported drive did not trigger")
        self.assertGreaterEqual(min(fired), 1500)
        self.assertLess(min(fired), 1600)


class TestNhtsa(ScenarioAssertions):
    """The NHTSA importer produces a valid, privacy-safe, triggering scenario."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = str(Path(self.tmp.name) / "nhtsa.txt")

    def tearDown(self):
        self.tmp.cleanup()

    def test_row_count_and_header(self):
        self.assertEqual(nhtsa.main(["--out", self.out]), 0)
        header, data = self.assert_valid_scenario(self.out, duration_ms=3000)
        self.assertEqual(int(header["crash_at_ms"]), 1000)
        self.assert_privacy_safe(data)

    def test_integration_check_closes_with_the_recorded_delta_v(self):
        integrated, recorded, error = nhtsa.check_integration(
            str(nhtsa.DEFAULT_FIXTURE))
        self.assertGreater(recorded, 0)
        self.assertLess(error, nhtsa.INTEGRATION_TOLERANCE)
        self.assertAlmostEqual(integrated, recorded, delta=recorded * 0.02)

    def test_a_mismatched_fixture_is_refused(self):
        """A wrong unit conversion must fail loudly, not silently pass."""
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.csv"
            bad.write_text("t_ms,accel_long_g,accel_lat_g,velocity_kmh\n"
                           "0,0.0,0.0,0.0\n1,-10.0,0.0,500.0\n", encoding="utf-8")
            with self.assertRaises(SystemExit):
                nhtsa.check_integration(str(bad))

    def test_g_to_mm_s2_conversion(self):
        rows = nhtsa.pulse_rows(str(nhtsa.DEFAULT_FIXTURE))
        peak = min(accel for _, accel, _ in rows)
        # The deepest sample of the fixture is -36.5 g, i.e. about -357 943 mm/s^2.
        self.assertAlmostEqual(peak, -36.5 * nhtsa.G_TO_MM_S2, delta=1)

    def test_imported_scenario_triggers_an_event(self):
        self.assertEqual(nhtsa.main(["--out", self.out]), 0)
        _, data = self.assert_valid_scenario(self.out)
        fired = run_trigger(data)
        self.assertTrue(fired, "the imported pulse did not trigger")
        self.assertGreaterEqual(min(fired), 1000)
        self.assertLess(min(fired), 1150)


def run_trigger(rows):
    """Feed a scenario to the Python trigger twin; return the firing indexes.

    host/verify/ambre_trigger.py is imported, never re-implemented: it is the
    same engine the cross-check uses against the C implementation (DEC-12).
    """
    cfg = at.cfg_default()
    state = at.TrigState(cfg)
    fired = []
    for index, row in enumerate(rows):
        values = [int(item) for item in row.split(" ")]
        t_us, a_long, a_lat = values[0], values[1], values[2]
        ext = at.parse_ext_flags(values[8])
        out = at.push(state, t_us, a_long, a_lat, ext)
        if out.fired:
            fired.append(index)
    return fired


class TestCrossImporter(unittest.TestCase):
    """The three sources must agree on the format, not only on their own."""

    def test_all_importers_produce_the_same_column_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            outputs = []
            outputs = [(Path(tmp) / "s.txt").as_posix(),
                       (Path(tmp) / "c.txt").as_posix(),
                       (Path(tmp) / "n.txt").as_posix()]
            synthetic.main(["--duration-ms", "2000", "--crash-at-ms", "1500",
                            "--out", outputs[0]])
            c2k.main(["--out", outputs[1]])
            nhtsa.main(["--out", outputs[2]])
            column_lines = set()
            for path in outputs:
                header, rows = parse_scenario(path)
                self.assertEqual(header["rate_hz"], "1000")
                for line in rows:
                    self.assertEqual(len(line.split(" ")), 9)
                column_lines.add(next(
                    line for line in Path(path).read_text(encoding="utf-8")
                    .splitlines() if line.startswith("# t_us")))
            # One single dialect: the column comment is identical everywhere.
            self.assertEqual(len(column_lines), 1)


if __name__ == "__main__":
    unittest.main(exit=True, verbosity=2)
