#!/usr/bin/env python3
"""Standalone tests for the Python trigger twin (no pytest needed).

Run:  python3 tests/test_trigger_py.py
Exit code = number of failed tests (0 = green).
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host" / "verify"))

import ambre_trigger as at  # noqa: E402

WINDOW = at.AMBRE_TRIG_MODE_WINDOW
ANCHORED = at.AMBRE_TRIG_MODE_ANCHORED


def _run(cfg, samples):
    """Run samples [(ts, a_long, a_lat, ext)] through a fresh engine."""
    st = at.TrigState(cfg)
    return [at.push(st, ts, al, alat, ext) for ts, al, alat, ext in samples]


def _ramp(cfg, accel, steps, step_us=1000, a_lat=0, start_us=0):
    """Constant-acceleration ramp of `steps` intervals."""
    samples = [(start_us, 0, 0, None)]
    for i in range(1, steps + 1):
        samples.append((start_us + i * step_us, accel, a_lat, None))
    return _run(cfg, samples)


def _fire_index(outs):
    for i, o in enumerate(outs):
        if o.fired:
            return i
    return -1


class TestDivision(unittest.TestCase):
    def test_idiv_trunc_positive(self):
        self.assertEqual(at.idiv_trunc(7, 2), 3)
        self.assertEqual(at.idiv_trunc(10, 5), 2)
        self.assertEqual(at.idiv_trunc(0, 5), 0)

    def test_idiv_trunc_negative(self):
        # C truncates toward zero; Python // would floor to -4 / -3.
        self.assertEqual(at.idiv_trunc(-7, 2), -3)
        self.assertEqual(at.idiv_trunc(7, -2), -3)
        self.assertEqual(at.idiv_trunc(-7, -2), 3)
        self.assertEqual(at.idiv_trunc(-1, 1000000), 0)
        self.assertNotEqual(at.idiv_trunc(-7, 2), -7 // 2)

    def test_idiv_trunc_zero_den(self):
        with self.assertRaises(ZeroDivisionError):
            at.idiv_trunc(1, 0)

    def test_thresholds_a8(self):
        self.assertEqual(at.mkmh_to_mm_s(8000), 2222)
        self.assertEqual(at.mkmh_to_mm_s(25000), 6944)
        self.assertEqual(at.mkmh_to_mm_s(800), 222)
        self.assertEqual(at.AMBRE_DV_TRIGGER_MM_S, 2222)
        self.assertEqual(at.AMBRE_DV_LOCK_MM_S, 6944)
        self.assertEqual(at.AMBRE_DV_T0_MM_S, 222)

    def test_int64_wrap(self):
        self.assertEqual(at.to_int64((1 << 63)), -(1 << 63))
        self.assertEqual(at.to_int64(-(1 << 63) - 1), (1 << 63) - 1)
        self.assertEqual(at.to_int64(5), 5)


class TestDec08(unittest.TestCase):
    def test_7_9_kmh_150ms_does_not_fire(self):
        # 2194 mm/s over 150 ms: 94 steps @15 mm/s + 56 steps @14 mm/s.
        cfg = at.cfg_default()
        samples = [(0, 0, 0, None)]
        for i in range(1, 151):
            acc = 15000 if i <= 94 else 14000
            samples.append((i * 1000, acc, 0, None))
        outs = _run(cfg, samples)
        self.assertFalse(any(o.fired for o in outs))
        self.assertLessEqual(max(o.win_long_mm_s for o in outs), 2222)

    def test_8_1_kmh_150ms_fires(self):
        outs = _ramp(at.cfg_default(), 15000, 150)  # 2250 mm/s
        self.assertTrue(any(o.fired for o in outs))
        self.assertEqual(next(o.reason for o in outs if o.fired), at.AMBRE_TRIG_DV_LONG)

    def test_8_1_kmh_151ms_does_not_fire_anchored(self):
        cfg = at.TrigConfig(mode=ANCHORED)
        samples = [(0, 0, 0, None)]
        for i in range(1, 151):
            samples.append((i * 1000, 14000, 0, None))  # dv = 2100 at 150 ms
        samples.append((151000, 150000, 0, None))       # dv = 2250 at 151 ms
        outs = _run(cfg, samples)
        self.assertEqual(outs[-1].dv_long_mm_s, 2250)
        self.assertFalse(any(o.fired for o in outs))
        # WINDOW mode must still fire: 2236 > 2222 inside a 150 ms window.
        w_outs = _run(at.TrigConfig(mode=WINDOW), samples)
        self.assertTrue(any(o.fired for o in w_outs))


class TestDec01Dec02(unittest.TestCase):
    def test_long_axis_trigger(self):
        outs = _ramp(at.cfg_default(), 20000, 150)
        i = _fire_index(outs)
        self.assertGreaterEqual(i, 0)
        self.assertEqual(outs[i].reason, at.AMBRE_TRIG_DV_LONG)

    def test_lat_axis_trigger(self):
        outs = _ramp(at.cfg_default(), 0, 150, a_lat=20000)
        i = _fire_index(outs)
        self.assertGreaterEqual(i, 0)
        self.assertEqual(outs[i].reason, at.AMBRE_TRIG_DV_LAT)

    def test_below_threshold_no_trigger(self):
        outs = _ramp(at.cfg_default(), 5000, 150)  # 750 mm/s
        self.assertFalse(any(o.fired for o in outs))


class TestDec03Restraint(unittest.TestCase):
    def test_restraint_fires_and_locks(self):
        cfg = at.cfg_default()
        samples = [(i * 1000, 0, 0, None) for i in range(20)]
        samples[10] = (10000, 0, 0, at.TrigExt(restraint_deployed=True))
        outs = _run(cfg, samples)
        self.assertTrue(outs[10].fired)
        self.assertEqual(outs[10].reason, at.AMBRE_TRIG_RESTRAINT)
        self.assertTrue(outs[10].locked)
        self.assertEqual(outs[10].t0_ts_us, 10000)  # T0 = signal instant

    def test_lock_persists_after_event(self):
        cfg = at.cfg_default()
        samples = [(i * 1000, 0, 0, None) for i in range(30)]
        samples[5] = (5000, 0, 0, at.TrigExt(restraint_deployed=True))
        outs = _run(cfg, samples)
        self.assertTrue(outs[-1].locked)


class TestDec04Lock(unittest.TestCase):
    def test_lock_without_front_restraint(self):
        cfg = at.TrigConfig(has_front_restraint=0)
        outs = _ramp(cfg, 90000, 150)  # ~13500 mm/s > 6944
        self.assertTrue(any(o.locked for o in outs))
        self.assertIn(at.AMBRE_TRIG_DV25_LOCK, [o.reason for o in outs])

    def test_no_lock_with_front_restraint(self):
        cfg = at.TrigConfig(has_front_restraint=1)
        outs = _ramp(cfg, 90000, 150)
        self.assertTrue(any(o.fired for o in outs))
        self.assertFalse(any(o.locked for o in outs))


class TestDec05T0(unittest.TestCase):
    def test_t0_is_onset_of_0_8kmh(self):
        # Ramp: the 20 ms window crosses 222 mm/s early, not at fire time.
        cfg = at.cfg_default()
        outs = _ramp(cfg, 20000, 150)
        fired = next(o for o in outs if o.fired)
        self.assertLess(fired.t0_ts_us, 150 * 1000)
        self.assertGreater(fired.t0_ts_us, 0)

    def test_external_t0_is_signal_instant(self):
        cfg = at.cfg_default()
        samples = [(i * 1000, 0, 0, None) for i in range(10)]
        samples[7] = (7000, 0, 0, at.TrigExt(vru_activated=True))
        outs = _run(cfg, samples)
        self.assertEqual(outs[7].t0_ts_us, 7000)


class TestDec06End(unittest.TestCase):
    def test_event_ends_when_window_falls(self):
        cfg = at.cfg_default()
        # Crash then silence: the 20 ms window must drop back to <= 222.
        samples = [(i * 1000, 30000, 0, None) for i in range(200)]
        for i in range(200, 400):
            samples.append((i * 1000, 0, 0, None))
        outs = _run(cfg, samples)
        self.assertTrue(any(o.fired for o in outs))
        self.assertTrue(any(o.event_end for o in outs))
        end_i = next(i for i, o in enumerate(outs) if o.event_end)
        fire_i = _fire_index(outs)
        self.assertGreater(end_i, fire_i)


class TestDec09Grouping(unittest.TestCase):
    def test_two_events_in_same_crash_group(self):
        cfg = at.cfg_default()
        samples = [(i * 1000, 0, 0, None) for i in range(4000)]
        # Two separated impacts ~1 s apart, both within 5 s window.
        for i in range(100, 160):
            samples[i] = (i * 1000, 40000, 0, None)
        for i in range(1200, 1260):
            samples[i] = (i * 1000, 40000, 0, None)
        outs = _run(cfg, samples)
        fires = [o for o in outs if o.fired]
        self.assertGreaterEqual(len(fires), 2)
        self.assertEqual(fires[0].event_number, 1)
        self.assertEqual(fires[1].event_number, 2)
        self.assertGreater(fires[1].time_since_first_ms, 0)


class TestDec10Vru(unittest.TestCase):
    def test_vru_fires_and_locks(self):
        samples = [(i * 1000, 0, 0, None) for i in range(10)]
        samples[4] = (4000, 0, 0, at.TrigExt(vru_activated=True))
        outs = _run(at.cfg_default(), samples)
        self.assertTrue(outs[4].fired)
        self.assertEqual(outs[4].reason, at.AMBRE_TRIG_VRU)
        self.assertTrue(outs[4].locked)


class TestDec11NeverLocked(unittest.TestCase):
    def test_offprofile_reasons_never_lock(self):
        for bit, reason in ((at.TrigExt(emergency_brake=True), at.AMBRE_TRIG_EMERG_BRAKE),
                            (at.TrigExt(aeb_intervention=True), at.AMBRE_TRIG_AEB),
                            (at.TrigExt(service_command=True), at.AMBRE_TRIG_SERVICE)):
            samples = [(i * 1000, 0, 0, None) for i in range(10)]
            samples[3] = (3000, 0, 0, bit)
            outs = _run(at.cfg_default(), samples)
            self.assertTrue(outs[3].fired)
            self.assertEqual(outs[3].reason, reason)
            self.assertFalse(outs[3].locked)
            self.assertFalse(outs[-1].locked)


class TestDec13Invalid(unittest.TestCase):
    def test_invalid_sample_excluded_from_dv(self):
        cfg = at.cfg_default()
        samples = [(0, 0, 0, None)]
        samples.append((1000, 50000, 0, at.TrigExt(sample_invalid=True)))
        samples.append((2000, 50000, 0, None))
        outs = _run(cfg, samples)
        # Sample 1 invalid -> no integration at that step.
        self.assertEqual(outs[1].dv_long_mm_s, 0)
        # Sample 2 integrates over its own dt only (1000 us).
        self.assertEqual(outs[2].dv_long_mm_s, 50)

    def test_invalid_never_triggers_alone(self):
        samples = [(i * 1000, 999999, 0, at.TrigExt(sample_invalid=True))
                   for i in range(200)]
        outs = _run(at.cfg_default(), samples)
        self.assertFalse(any(o.fired for o in outs))


class TestDeterminismAndScale(unittest.TestCase):
    def _scenario(self):
        import random
        rng = random.Random(7)
        samples = [(0, 0, 0, None)]
        t = 0
        for _ in range(2000):
            t += rng.randint(500, 1500)
            samples.append((t, rng.randint(-80000, 80000), rng.randint(-5000, 5000), None))
        return samples

    def test_determinism(self):
        a = _run(at.cfg_default(), self._scenario())
        b = _run(at.cfg_default(), self._scenario())
        self.assertEqual([vars(o) for o in a], [vars(o) for o in b])

    def test_200k_samples_no_overflow(self):
        cfg = at.cfg_default()
        st = at.TrigState(cfg)
        ts = 0
        last = None
        for i in range(200000):
            ts += 1000
            acc = 50000 if (i // 500) % 2 == 0 else -50000
            last = at.push(st, ts, acc, acc // 2, None)
        self.assertIsNotNone(last)
        self.assertLess(at._INT64_MIN, last.dv_long_mm_s)
        self.assertGreater(at._INT64_MAX, last.dv_long_mm_s)
        self.assertGreaterEqual(last.win_long_mm_s, 0)


class TestDtClampAndGaps(unittest.TestCase):
    def test_dt_clamped_to_10ms(self):
        cfg = at.cfg_default()
        # Gap of 100 ms -> integration must use only 10 ms.
        samples = [(0, 0, 0, None), (100000, 1000000, 0, None)]
        outs = _run(cfg, samples)
        # 1e6 mm/s^2 * 10000 us / 1e6 = 10000 mm/s
        self.assertEqual(outs[1].dv_long_mm_s, 10000)

    def test_non_monotonic_ts_no_integration(self):
        cfg = at.cfg_default()
        samples = [(5000, 0, 0, None), (1000, 100000, 0, None)]
        outs = _run(cfg, samples)
        self.assertEqual(outs[1].dv_long_mm_s, 0)


class TestEventClosed(unittest.TestCase):
    def test_event_closed_resets_event(self):
        st = at.TrigState(at.cfg_default())
        at.push(st, 1000, 0, 0, None)
        at.push(st, 2000, 0, 0, at.TrigExt(restraint_deployed=True))
        self.assertTrue(st.in_event)
        at.event_closed(st, 3000)
        self.assertFalse(st.in_event)
        self.assertEqual(st.last_event_end_ts_us, 3000)


def main():
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return len(result.failures) + len(result.errors)


if __name__ == "__main__":
    sys.exit(main())
