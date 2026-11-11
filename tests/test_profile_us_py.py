#!/usr/bin/env python3
"""Proof for the US EDR profile (49 CFR Part 563) and its record size.

Run:  python3 tests/test_profile_us_py.py
Exit code = number of failed tests (0 = green).

The C twin is tests/test_policy_us.c: the two suites assert the same numbers,
so a change on either side breaks one of them.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from host.profiles import us_profile as up  # noqa: E402

# Values asserted identically in tests/test_policy_us.c.
FROZEN_LAYOUT_BYTES = 4032      # 32 B header + 200 points x 20 B
GENERIC_MODEL_BYTES = 16020     # 20 B header + 200 x 20 signals x 4 B
FROZEN_LAYOUT_BLOCKS = 9
GENERIC_MODEL_BLOCKS = 35
LARGEST_RECORD = 7360           # 16 blocks x 460 B


class TestFrozenConstants(unittest.TestCase):
    """The format constants the size model is built on (docs/FORMAT.md)."""

    def test_block_geometry(self):
        self.assertEqual(up.BLOCK_SIZE, 512)
        self.assertEqual(up.BLOCK_PAYLOAD_MAX, 460)
        self.assertEqual(up.BLOCK_SIZE - up.BLOCK_HEADER_BYTES - up.BLOCK_CRC_BYTES,
                         up.BLOCK_PAYLOAD_MAX)

    def test_record_geometry(self):
        self.assertEqual(up.REC_MAX_BLOCKS, 16)
        self.assertEqual(up.largest_record_payload(), LARGEST_RECORD)

    def test_class_a_structures(self):
        self.assertEqual(up.CLASS_A_POINT_BYTES, 20)
        self.assertEqual(up.EMERG_POINTS, 21)
        self.assertEqual(up.emerg_payload_capacity(), 420)


class TestProfiles(unittest.TestCase):
    """Window and rate of each profile, as the cahier states them."""

    def test_eu_is_5s_at_2hz(self):
        eu = up.eu_profile()
        self.assertEqual(eu.window_ms, 5000)
        self.assertEqual(eu.rate_hz, 2)
        self.assertEqual(eu.points, 10)
        self.assertEqual(eu.period_ms, 500)

    def test_us_is_20s_at_10hz(self):
        us = up.us_profile()
        self.assertEqual(us.window_ms, 20000)
        self.assertEqual(us.rate_hz, 10)
        self.assertEqual(us.points, 200)
        self.assertEqual(us.period_ms, 100)

    def test_us_has_twenty_times_more_points_than_eu(self):
        self.assertEqual(up.us_profile().points, 20 * up.eu_profile().points)


class TestSubsampling(unittest.TestCase):
    """The US class A is derived from class B by decimation (cahier 6.2)."""

    def test_decimation_factors(self):
        plan = up.subsampling_plan(up.us_profile())
        self.assertEqual(plan["accel"], 100)     # 1 kHz -> 10 Hz
        self.assertEqual(plan["dynamics"], 10)   # 100 Hz -> 10 Hz
        self.assertEqual(plan["states"], 1)      # 10 Hz -> 10 Hz

    def test_eu_plan(self):
        plan = up.subsampling_plan(up.eu_profile())
        self.assertEqual(plan["accel"], 500)     # 1 kHz -> 2 Hz
        self.assertEqual(plan["dynamics"], 50)
        self.assertEqual(plan["states"], 5)

    def test_non_divisor_is_refused(self):
        with self.assertRaises(ValueError):
            up.decimation_factor(1000, 3)

    def test_zero_rate_is_refused(self):
        with self.assertRaises(ValueError):
            up.decimation_factor(1000, 0)


class TestUsRecordSize(unittest.TestCase):
    """The measured size of a US EMERG, in both encodings."""

    def test_frozen_layout_is_4032_bytes(self):
        self.assertEqual(up.frozen_us_emerg_bytes(), FROZEN_LAYOUT_BYTES)
        self.assertEqual(up.EMERG_FIXED_HEADER_BYTES
                         + up.US_POINTS * up.CLASS_A_POINT_BYTES,
                         FROZEN_LAYOUT_BYTES)

    def test_generic_model_is_16020_bytes(self):
        self.assertEqual(up.us_profile().payload_bytes, GENERIC_MODEL_BYTES)
        self.assertEqual(up.US_POINTS * up.US_SIGNALS * up.US_VALUE_BYTES
                         + up.US_HEADER_BYTES, GENERIC_MODEL_BYTES)

    def test_block_counts(self):
        self.assertEqual(up.blocks_needed(FROZEN_LAYOUT_BYTES), FROZEN_LAYOUT_BLOCKS)
        self.assertEqual(up.blocks_needed(GENERIC_MODEL_BYTES), GENERIC_MODEL_BLOCKS)

    def test_neither_fits_one_block(self):
        self.assertFalse(up.fits_one_block(FROZEN_LAYOUT_BYTES))
        self.assertFalse(up.fits_one_block(GENERIC_MODEL_BYTES))

    def test_only_the_frozen_layout_fits_one_record(self):
        self.assertTrue(up.fits_one_record(FROZEN_LAYOUT_BYTES))
        self.assertFalse(up.fits_one_record(GENERIC_MODEL_BYTES))

    def test_record_counts(self):
        self.assertEqual(up.records_needed(FROZEN_LAYOUT_BYTES), 1)
        self.assertEqual(up.records_needed(GENERIC_MODEL_BYTES), 3)

    def test_verdict_dict_matches_the_c_function(self):
        v = up.verdict()
        self.assertEqual(v["frozen_layout_bytes"], FROZEN_LAYOUT_BYTES)
        self.assertEqual(v["generic_model_bytes"], GENERIC_MODEL_BYTES)
        self.assertEqual(v["frozen_blocks"], FROZEN_LAYOUT_BLOCKS)
        self.assertEqual(v["generic_blocks"], GENERIC_MODEL_BLOCKS)
        self.assertFalse(v["frozen_fits_block"])
        self.assertFalse(v["generic_fits_block"])
        self.assertTrue(v["frozen_fits_record"])
        self.assertFalse(v["generic_fits_record"])


class TestFrozenStructLimit(unittest.TestCase):
    """The frozen struct covers 2.1 s of the US window, not the whole 20 s."""

    def test_emerg_holds_21_points(self):
        self.assertEqual(up.points_in_emerg(), 21)

    def test_21_points_are_2100_ms_at_10_hz(self):
        self.assertEqual(up.window_covered_ms(21, 10), 2100)

    def test_21_points_are_10500_ms_at_2_hz(self):
        self.assertEqual(up.window_covered_ms(21, 2), 10500)

    def test_us_window_exceeds_the_frozen_capacity(self):
        covered = up.window_covered_ms(up.points_in_emerg(), up.US_RATE_HZ)
        self.assertLess(covered, up.us_profile().window_ms)
        self.assertEqual(up.us_profile().window_ms // covered, 9)

    def test_points_needed_by_the_us_window(self):
        need = up.us_profile().points * up.CLASS_A_POINT_BYTES
        self.assertEqual(need, FROZEN_LAYOUT_BYTES - up.EMERG_FIXED_HEADER_BYTES)
        self.assertGreater(need, up.emerg_payload_capacity())


class TestReport(unittest.TestCase):
    """The rendered report states the verdict, and its numbers are the ones
    asserted above (no drift between the model and what a human reads)."""

    def test_report_contains_the_measured_sizes(self):
        text = up.report()
        self.assertIn(str(FROZEN_LAYOUT_BYTES), text)
        self.assertIn(str(GENERIC_MODEL_BYTES), text)
        self.assertIn("multi-block is mandatory", text)

    def test_describe_is_consistent(self):
        d = up.describe(up.us_profile())
        self.assertEqual(d["payload_bytes"], GENERIC_MODEL_BYTES)
        self.assertEqual(d["blocks_needed"], GENERIC_MODEL_BLOCKS)
        self.assertEqual(d["records_needed"], 3)
        self.assertEqual(d["largest_record_payload"], LARGEST_RECORD)


if __name__ == "__main__":
    unittest.main(exit=True, verbosity=2)
