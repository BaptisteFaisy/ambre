#!/usr/bin/env python3
"""Generator of test vectors (.vec format) for the Ambre crash trigger engine.

Produces reproducible scenarios with noisy accelerations, configurable crashes,
DEC-08 boundary cases, external triggers, cadence gaps, and invalid samples.
Ensures at least 30% of scenarios trigger.
"""

import argparse
import random
import sys
from pathlib import Path
from typing import List, TextIO

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ambre_trigger as at  # noqa: E402


def _write_scenario_header(f: TextIO, scen_id: int, mode: int,
                           has_front_restraint: int = 1) -> None:
    """Write S line for scenario definition."""
    # S <id> <mode> <dv_trigger_mm_s> <dv_lock_mm_s> <dv_t0_mm_s> <win_trigger_ms> \
    #   <win_t0_long_ms> <win_t0_lat_ms> <has_front_restraint> <same_crash_window_ms>
    f.write(
        f"S {scen_id} {mode} 2222 6944 222 150 20 5 {has_front_restraint} 5000\n"
    )


def _write_ramp(f: TextIO, accel: int, steps: int, t_us: int) -> int:
    """Write `steps` 1 ms samples at constant accel; return the next timestamp."""
    for _ in range(steps):
        f.write(f"A {t_us} {accel} 0 0\n")
        t_us += 1000
    return t_us


def _gen_dec08_cases(f: TextIO, start_id: int) -> int:
    """Generate the three DEC-08 boundary scenarios with exact delta-v.

    Per-sample integration is idiv_trunc(accel * 1000, 1e6), i.e. accel/1000
    mm/s per 1 ms step, so accel 15000 -> 15 mm/s and 14000 -> 14 mm/s.
    A settling sample at t = 0 gives the first ramp sample a real dt (A3 only
    integrates from i > 0) and anchors ANCHORED mode at t = 0.
    """
    scen_id = start_id
    settle = "A 0 0 0 0\n"

    # Case 1: 7.9 km/h = 2194 mm/s in exactly 150 ms -> must NOT trigger.
    # 94 steps @15 mm/s + 56 steps @14 mm/s = 1410 + 784 = 2194 (< 2222).
    _write_scenario_header(f, scen_id, mode=0)
    f.write(settle)
    t = _write_ramp(f, 15000, 94, 1000)
    _write_ramp(f, 14000, 56, t)
    f.write("E\n")
    scen_id += 1

    # Case 2: 8.1 km/h = 2250 mm/s in exactly 150 ms -> must trigger.
    # 150 steps @15 mm/s = 2250 (> 2222).
    _write_scenario_header(f, scen_id, mode=0)
    f.write(settle)
    _write_ramp(f, 15000, 150, 1000)
    f.write("E\n")
    scen_id += 1

    # Case 3: 8.1 km/h = 2250 mm/s reached in 151 ms -> ANCHORED must NOT fire.
    # 150 steps @14 mm/s -> dv = 2100 at t = 150 ms, then one 150 mm/s step at
    # t = 151 ms -> dv = 2250, but 151 ms after the t = 0 anchor.
    # WINDOW mode still fires (2236 > 2222 in the 150 ms window): that is the
    # whole point of the ANCHORED reading.
    _write_scenario_header(f, scen_id, mode=1)
    f.write(settle)
    t = _write_ramp(f, 14000, 150, 1000)
    f.write(f"A {t} 150000 0 0\n")
    f.write("E\n")
    scen_id += 1

    return scen_id


CRASH_CATEGORIES = ("crash_long", "crash_lat", "crash_lock", "external")
QUIET_CATEGORIES = ("quiet", "sub_threshold", "noisy_gaps")


def _sample_values(cat: str, i: int, crash_start: int, ext_fired: bool,
                   rng: random.Random) -> tuple[int, int, int, bool]:
    """Pick (a_long, a_lat, ext_flags, ext_fired) for one sample."""
    ext_flags = 0x20 if rng.random() < 0.02 else 0
    a_long = rng.randint(-300, 300)
    a_lat = rng.randint(-300, 300)

    if cat == "crash_long" and crash_start <= i <= crash_start + 80:
        a_long = rng.randint(30000, 60000)
    elif cat == "crash_lat" and crash_start <= i <= crash_start + 80:
        a_lat = rng.randint(30000, 60000)
    elif cat == "crash_lock" and crash_start <= i <= crash_start + 120:
        a_long = rng.randint(60000, 120000)
    elif cat == "external" and i == crash_start and not ext_fired:
        ext_flags |= rng.choice([0x01, 0x02, 0x04, 0x08, 0x10])
        ext_fired = True
    elif cat == "sub_threshold":
        a_long = rng.randint(0, 12000)
    return a_long, a_lat, ext_flags, ext_fired


def _gen_random_scenario(f: TextIO, scen_id: int, rng: random.Random,
                         force_trigger: bool, default_mode: int) -> bool:
    """Generate one randomized scenario. Return whether it should trigger."""
    mode = default_mode if default_mode in (0, 1) else rng.choice([0, 0, 0, 1])
    _write_scenario_header(f, scen_id, mode, rng.choice([0, 1]))

    num_samples = rng.randint(150, 400)
    t_us = 1000000 + rng.randint(0, 50000)
    cat = rng.choice(CRASH_CATEGORIES if force_trigger else QUIET_CATEGORIES)
    crash_start = rng.randint(20, 80)
    ext_fired = False

    for i in range(num_samples):
        t_us += rng.randint(5000, 25000) if rng.random() < 0.03 else rng.randint(950, 1050)
        a_long, a_lat, ext_flags, ext_fired = _sample_values(
            cat, i, crash_start, ext_fired, rng)
        f.write(f"A {t_us} {a_long} {a_lat} {ext_flags}\n")

    f.write("E\n")
    return cat in CRASH_CATEGORIES


def _count_actual_triggers(out_path: str) -> tuple[int, int]:
    """Parse the written .vec and count scenarios that ACTUALLY fire."""
    scenarios = at.parse_vec(out_path)
    fired = sum(1 for s in scenarios if any(o.fired for o in at.run_scenario(s)))
    return len(scenarios), fired


def generate_vectors(out_path: str, count: int, seed: int,
                     mode: int = -1) -> tuple[int, int]:
    """Generate count scenarios to out_path, ensuring >= 30% actually trigger.

    Returns (total_scenarios, actual_triggered) where actual_triggered is
    measured by running the Python engine, not predicted by the generator.
    """
    rng = random.Random(seed)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# Ambre trigger test vectors\n")
        scen_id = 1
        scen_id = _gen_dec08_cases(f, scen_id)
        dec08_count = scen_id - 1
        # Heuristic running estimate used only to bias generation toward triggers.
        est_triggered = 1

        remaining = max(0, count - dec08_count)
        for _ in range(remaining):
            force = (est_triggered / scen_id < 0.42) or (rng.random() < 0.52)
            if _gen_random_scenario(f, scen_id, rng, force, mode):
                est_triggered += 1
            scen_id += 1

    return _count_actual_triggers(out_path)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate Ambre trigger .vec test vectors")
    parser.add_argument("--scenarios", type=int, default=100, help="Number of scenarios")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--out", type=str, required=True, help="Output .vec path")
    parser.add_argument("--mode", type=int, default=-1, choices=[-1, 0, 1],
                        help="Trigger mode override (-1 for mixed)")
    args = parser.parse_args()

    total, triggered = generate_vectors(args.out, args.scenarios, args.seed, args.mode)
    pct = (triggered / total * 100.0) if total > 0 else 0.0
    print(f"Generated {total} scenarios to {args.out}: "
          f"{triggered} actually trigger ({pct:.1f}%)")
    if pct < 30.0:
        sys.stderr.write(f"error: triggered proportion {pct:.1f}% is below 30%\n")
        sys.exit(1)


if __name__ == "__main__":
    main()

