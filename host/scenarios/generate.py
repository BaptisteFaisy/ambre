#!/usr/bin/env python3
"""Ambre scenario generator (docs/FORMAT.md section 11).

Deterministic, privacy-safe synthetic drives for the EDR replay and
exploration tools: a plain-text scenario file with a key=value header, one
comment line naming the columns, then one sample line per millisecond. The
1 kHz stream carries the fast signals; the slow signals (pedals, engine
speed, steering angle) are refreshed at 100 Hz and hold their last value in
between.

Measurement fidelity: the impact pulse integrates to the requested delta-v
exactly and every impact sample is a multiple of 1000 mm/s^2, so the integer
rectangle rule of the trigger engine integrates it without rounding loss; the
speed column is the integral of the longitudinal acceleration; the quiet
approach of the borderline presets integrates to a zero delta-v before the
impact, so the measured excursion is the impact itself.

Privacy: no VIN, no position, no absolute date -- only a name, a seed and
durations relative to the start of the drive.

Usage:
    python3 host/scenarios/generate.py --preset urban-frontal --seed 1234 \
        --duration-ms 30000 --crash-at-ms 25000 --out /tmp/urban-frontal.txt
    python3 host/scenarios/generate.py --list-presets
"""

from __future__ import annotations

import argparse
import math
import random
import sys

SCENARIO_VERSION = 1
COLUMNS = ("# t_us a_long_mm_s2 a_lat_mm_s2 speed_kmh_x4 throttle brake "
           "rpm steer_x2 ext_flags")
HEADER_KEYS = ("name", "seed", "rate_hz", "duration_ms", "crash_at_ms")

BASE_RATE_HZ = 1000          # one sample per millisecond, FORMAT.md section 11
SLOW_SIGNAL_HZ = 100         # pedals, rpm, steering angle refresh rate
DEFAULT_SEED = 1
DEFAULT_DV_KMH = 12.0
DEFAULT_PULSE_MS = 120
TAIL_MS = 500                # steady coast before the impact
RESIDUAL_DELAY_MS = 1000    # the driver brakes after the impact
RESIDUAL_MM_S2 = -4000      # residual deceleration after the impact
RESIDUAL_MAX_MS = 4000
RESTRAINT_WINDOW_MS = 1000
MAX_PEAK_MM_S2 = 200000      # int32 range of the class B acceleration field

# ext_flags bits (docs/FORMAT.md section 11)
EXT_RESTRAINT = 1 << 0
EXT_VRU = 1 << 1
EXT_EMERGENCY_BRAKE = 1 << 2
EXT_AEB = 1 << 3
EXT_SERVICE = 1 << 4
EXT_INVALID = 1 << 5

# Vehicle / drive model constants
IDLE_RPM = 800
MAX_RPM = 6000
SPEED_TAU_MS = 1500.0
WHEELBASE_M = 2.7
STEERING_RATIO = 15.0
STEER_SLEW_X2 = 0.6          # 0.5 deg units per millisecond
NOISE_MM_S2 = 120
LAT_CAP_MM_S2 = 3500
STEP_TO_MM_S2 = 1000         # one pulse step of 1 mm/s is 1000 mm/s^2
QUIET_HALF_MS = (600, 1400)
QUIET_ACCELS = (-1200, -800, -400, 400, 800, 1200)

PROFILES = {
    "eu": {"rate_hz": 1000, "duration_ms": 30_000,
           "note": "EU R160 class B: 1 kHz accelerations, 100 Hz dynamics"},
    "us": {"rate_hz": 1000, "duration_ms": 20_000,
           "note": "US 49 CFR 563: 20 s window, class A derived at 10 Hz"},
}

PRESETS = {
    "urban-frontal": {
        "description": "30 s urban drive, 12 km/h frontal impact at 25 s, restraint fires",
        "duration_ms": 30_000,
        "crashes": [dict(at=25_000, dv_kmh=12.0, pulse_ms=120, shape="crash", sign=-1, restraint=True)],
    },
    "side-impact": {
        "description": "20 s urban drive, 9 km/h lateral impact at 12 s, no restraint",
        "duration_ms": 20_000,
        "crashes": [dict(at=12_000, dv_kmh=9.0, pulse_ms=120, shape="crash", axis="lat", sign=1)],
    },
    "rear-impact": {
        "description": "20 s urban drive, 10 km/h rear impact (positive long.) at 11 s",
        "duration_ms": 20_000,
        "crashes": [dict(at=11_000, dv_kmh=10.0, pulse_ms=120, shape="crash", sign=1)],
    },
    "no-crash": {
        "description": "clean 20 s urban drive, no impact at all",
        "duration_ms": 20_000,
        "crashes": [],
    },
    "borderline-7.9": {
        "description": "7.9 km/h in 150 ms: below DEC-01, must not trigger",
        "duration_ms": 12_000, "quiet": True, "residual": 0,
        "crashes": [dict(at=8000, dv_kmh=7.9, pulse_ms=150, shape="flat")],
    },
    "borderline-8.1": {
        "description": "8.1 km/h in 150 ms: above DEC-01, must trigger",
        "duration_ms": 12_000, "quiet": True, "residual": 0,
        "crashes": [dict(at=8000, dv_kmh=8.1, pulse_ms=150, shape="flat")],
    },
    "borderline-151ms": {
        "description": "8.1 km/h reached in 151 ms: ANCHORED mode must not trigger",
        "duration_ms": 12_000, "quiet": True, "residual": 0,
        "crashes": [dict(at=8000, dv_kmh=8.1, pulse_ms=151, shape="late")],
    },
    "multiple-events": {
        "description": "two impacts 1.8 s apart: one accident, event 1 then event 2 (DEC-09)",
        "duration_ms": 20_000,
        "crashes": [
            dict(at=10_000, dv_kmh=9.0, pulse_ms=120, shape="crash", sign=-1, restraint=True),
            dict(at=11_800, dv_kmh=7.0, pulse_ms=120, shape="crash", sign=-1),
        ],
    },
    "locked-airbag": {
        "description": "30 km/h over 300 ms with restraint deployment: event locked (DEC-03)",
        "duration_ms": 15_000,
        "crashes": [dict(at=10_000, dv_kmh=30.0, pulse_ms=300, shape="crash", sign=-1, restraint=True)],
    },
}


def _clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


def _round_exact(values, target):
    """Round floats to ints while keeping the sum equal to `target`."""
    floors = [int(math.floor(v)) for v in values]
    extra = int(target) - sum(floors)
    order = sorted(range(len(values)), key=lambda k: values[k] - floors[k], reverse=True)
    for k in order[:max(0, min(extra, len(values)))]:
        floors[k] += 1
    return floors


def _shape_weights(count, shape):
    """Normalised weights of the impact shape, in [0, 1]."""
    if shape == "flat":
        return [1.0] * count
    if shape == "ramp":
        return [(k + 1) / float(count) for k in range(count)]
    if shape == "sine":
        return [math.sin(math.pi * (k + 0.5) / count) for k in range(count)]
    # "crash": trapezoid, short attack and release around a flat plateau.
    ramp = max(1, int(round(0.12 * count)))
    weights = []
    for k in range(count):
        attack = min(1.0, (k + 0.5) / ramp)
        release = min(1.0, (count - k - 0.5) / ramp)
        weights.append(attack * release)
    return weights


def pulse_steps(dv_kmh, pulse_ms, shape):
    """Impact increments, in mm/s per millisecond, summing to the delta-v.

    The returned integers are multiples of 1 mm/s; the acceleration samples
    derived from them are multiples of 1000 mm/s^2, so the integer rectangle
    rule of the trigger engine integrates them without any rounding loss. The
    "late" shape holds a flat level for `pulse_ms - 1` milliseconds and puts
    the last increment on the final millisecond: the delta-v threshold is then
    crossed only at the 151st millisecond (DEC-08 case 3).
    """
    count = max(1, int(pulse_ms))
    total = int(round(abs(dv_kmh) * 1000.0 / 3.6))
    if shape == "late":
        flat = max(1, total // count)
        steps = [flat] * (count - 1)
        steps.append(total - flat * (count - 1))
        return steps
    weights = _shape_weights(count, shape)
    scaled = [total * w / sum(weights) for w in weights]
    return _round_exact(scaled, total)


def plan_drive(rng, end_ms, quiet):
    """Driving plan: list of (start_ms, end_ms, command, steer_target_x2).

    In the quiet mode the segments come in pairs of equal length and opposite
    acceleration, so the integrated delta-v returns to zero at the end of each
    pair: the impact excursion is then measured from a true rest anchor.
    """
    if quiet:
        return _quiet_plan(rng, end_ms)
    plan = []
    start = 0
    while start < end_ms:
        length = rng.randint(1800, 4000)
        end = min(start + length, end_ms)
        target_speed = rng.uniform(6.0, 15.5) * 1000.0
        steer = rng.choice((-1, 1)) * rng.randint(0, 160)
        plan.append((start, end, target_speed, steer))
        start = end
    return plan


def _quiet_plan(rng, end_ms):
    """Delta-v neutral plan: an acceleration always followed by its opposite."""
    plan = []
    start = 0
    remaining = end_ms
    while remaining >= 2:
        half = min(rng.randint(*QUIET_HALF_MS), remaining // 2)
        if half < 1:
            break
        accel = rng.choice(QUIET_ACCELS)
        plan.append((start, start + half, accel, 0))
        plan.append((start + half, start + 2 * half, -accel, 0))
        start += 2 * half
        remaining -= 2 * half
    if remaining > 0:
        plan.append((start, start + remaining, 0, 0))
    return plan


def _plan_entry(plan, state, index):
    """Current plan segment, advancing the cursor when the index moves on."""
    while state["pi"] + 1 < len(plan) and index >= plan[state["pi"]][1]:
        state["pi"] += 1
    return plan[state["pi"]]


def _lateral_mm_s2(v_mm_s, steer_x2):
    """Lateral acceleration implied by the steering angle and the speed."""
    road_deg = (steer_x2 / 2.0) / STEERING_RATIO
    curvature = math.tan(math.radians(road_deg)) / WHEELBASE_M
    accel = (v_mm_s / 1000.0) ** 2 * curvature * 1000.0
    return int(_clamp(accel, -LAT_CAP_MM_S2, LAT_CAP_MM_S2))


def _drive_step(plan, state, rng, index, quiet):
    """One millisecond of pre-crash driving; updates the state."""
    _, _, command, steer_t = _plan_entry(plan, state, index)
    speed = state["v"]
    if quiet:
        accel_t = command
    else:
        accel_t = (command - speed) * 1000.0 / SPEED_TAU_MS
    accel_t = _clamp(accel_t, -3000.0, 2000.0)
    if speed <= 0.0 and accel_t < 0:
        accel_t = 0.0
    noise = 0 if quiet else rng.randint(-NOISE_MM_S2, NOISE_MM_S2)
    a_long = int(_clamp(accel_t + noise, -6000, 3500))
    state["steer"] += _clamp(steer_t - state["steer"], -STEER_SLEW_X2, STEER_SLEW_X2)
    a_lat = _lateral_mm_s2(speed, state["steer"])
    brake = 1 if a_long < -600 else 0
    throttle = 0 if brake else _clamp(int(round(a_long / 30.0)), 0, 100)
    return a_long, a_lat, throttle, brake


def _rpm(v_mm_s, throttle, rng):
    """Engine speed, from the road speed and the throttle position."""
    kmh = v_mm_s * 0.0036
    value = IDLE_RPM + kmh * 42.0 + throttle * 6.0 + rng.randint(-25, 25)
    return int(_clamp(round(value), IDLE_RPM, MAX_RPM))


def build_crash_map(crashes, n_ms):
    """index -> (axis, acceleration) for every sample inside an impact."""
    accel = {}
    for crash in crashes:
        steps = pulse_steps(crash["dv_kmh"], crash["pulse_ms"], crash["shape"])
        for offset, step in enumerate(steps):
            index = crash["at"] + offset
            if 0 <= index < n_ms:
                accel[index] = (crash["axis"], crash["sign"] * step * STEP_TO_MM_S2)
    return accel


def flag_spans(crashes):
    """(start_ms, end_ms, bit) spans OR-ed into ext_flags."""
    spans = []
    for crash in crashes:
        if crash["pre_brake"]:
            spans.append((max(0, crash["at"] - 600), crash["at"], EXT_EMERGENCY_BRAKE))
        if crash["aeb"]:
            spans.append((max(0, crash["at"] - 400), crash["at"], EXT_AEB))
        if crash["restraint"]:
            spans.append((crash["at"] + 25,
                          crash["at"] + 25 + RESTRAINT_WINDOW_MS, EXT_RESTRAINT))
    return spans


def build_flags(n_ms, spans):
    """One ext_flags value per millisecond."""
    flags = [0] * n_ms
    for start, end, bit in spans:
        for i in range(max(0, start), min(end, n_ms)):
            flags[i] |= bit
    return flags


def _sample_at(i, accel_map, plan, state, rng, cfg):
    """Acceleration and pedal state for one millisecond."""
    if i in accel_map:
        axis, value = accel_map[i]
        if axis == "long":
            return value, 0, 0, 1
        return 0, value, 0, 0
    if i >= cfg["tail_start"]:
        state["steer"] = 0.0
        return _post_impact(i, state, cfg)
    return _drive_step(plan, state, rng, i, cfg["quiet"])


def _post_impact(i, state, cfg):
    """Residual braking after the impact, then the vehicle comes to rest."""
    if i < cfg["residual_start"] or i >= cfg["residual_end"]:
        return 0, 0, 0, 0
    if state["v"] <= 0.0:
        return 0, 0, 0, 0
    return cfg["residual"], 0, 0, 1


def simulate(cfg, rng):
    """Run the model; returns one tuple of 8 signals per sample."""
    n_ms = cfg["duration_ms"]
    plan = plan_drive(rng, cfg["tail_start"], cfg["quiet"])
    accel_map = build_crash_map(cfg["crashes"], n_ms)
    flags = build_flags(n_ms, cfg["flag_spans"])
    # The first plan target is the speed at t = 0: no start transient.
    speed0 = cfg["start_speed_mm_s"] if (cfg["quiet"] or not plan) else plan[0][2]
    state = {"v": float(speed0), "steer": 0.0, "pi": 0}
    slow_period = max(1, BASE_RATE_HZ // max(1, cfg["slow_hz"]))
    held = {"throttle": 0, "brake": 0, "rpm": IDLE_RPM, "steer": 0}
    rows = []

    for i in range(n_ms):
        a_long, a_lat, throttle, brake = _sample_at(i, accel_map, plan, state,
                                                    rng, cfg)
        state["v"] = max(0.0, state["v"] + a_long * 0.001)
        if i % slow_period == 0:
            held["throttle"] = throttle
            held["brake"] = brake
            held["rpm"] = _rpm(state["v"], throttle, rng)
            held["steer"] = int(round(state["steer"]))
        speed_x4 = int(_clamp(round(state["v"] * 0.0144), 0, 65_535))
        rows.append((a_long, a_lat, speed_x4, held["throttle"], held["brake"],
                     held["rpm"], held["steer"], flags[i]))
    return rows


def render(cfg, rows):
    """Format the scenario exactly as docs/FORMAT.md section 11 prescribes."""
    step_us = 1_000_000 // cfg["rate_hz"]
    lines = ["# ambre scenario v%d" % SCENARIO_VERSION]
    lines += ["%s=%s" % (key, cfg[key]) for key in HEADER_KEYS]
    lines.append(COLUMNS)
    for index, row in enumerate(rows):
        lines.append(" ".join(str(value) for value in (index * step_us,) + row))
    return "\n".join(lines) + "\n"


def normalize_crashes(crashes):
    """Fill in the default shape of every impact."""
    defaults = dict(dv_kmh=DEFAULT_DV_KMH, pulse_ms=DEFAULT_PULSE_MS,
                    shape="crash", axis="long", sign=-1, restraint=False,
                    aeb=False, pre_brake=False)
    normalized = []
    for crash in crashes:
        entry = dict(defaults)
        entry.update(crash)
        normalized.append(entry)
    return normalized


def _crash_bounds(crashes, duration):
    """First impact index, and the residual braking window."""
    if not crashes:
        return duration, duration, duration
    first = min(c["at"] for c in crashes)
    last_end = max(c["at"] + c["pulse_ms"] for c in crashes)
    residual_start = last_end + RESIDUAL_DELAY_MS
    return first, residual_start, residual_start + RESIDUAL_MAX_MS


def _resolve_crashes(args, preset, duration):
    """Impact list requested on the command line, honouring the preset."""
    if args.preset is not None:
        crashes = normalize_crashes(preset.get("crashes", []))
        if args.crash_at_ms is None:
            return crashes
        if args.crash_at_ms <= 0:
            return []
        if crashes:
            crashes[0]["at"] = args.crash_at_ms
        else:
            crashes = normalize_crashes([dict(at=args.crash_at_ms)])
    else:
        at = args.crash_at_ms
        if at is None:
            at = max(0, duration - 5000)
        crashes = [] if at <= 0 else normalize_crashes([dict(at=at)])
    if args.dv_kmh is not None:
        for crash in crashes:
            crash["dv_kmh"] = args.dv_kmh
    return crashes


def build_config(args):
    """Turn the command line into the full generation configuration."""
    preset = PRESETS.get(args.preset, {}) if args.preset else {}
    profile = PROFILES[args.profile]
    duration = args.duration_ms if args.duration_ms is not None else (
        preset.get("duration_ms") or profile["duration_ms"])
    rate = args.rate_hz if args.rate_hz is not None else profile["rate_hz"]
    crashes = _resolve_crashes(args, preset, duration)
    first, residual_start, residual_end = _crash_bounds(crashes, duration)
    cfg = {
        "name": args.preset or "custom",
        "seed": args.seed,
        "rate_hz": rate,
        "duration_ms": duration,
        "crash_at_ms": crashes[0]["at"] if crashes else 0,
        "crashes": crashes,
        "first_crash": first,
        "tail_start": max(0, first - TAIL_MS),
        "residual_start": residual_start,
        "residual_end": residual_end,
        "residual": preset.get("residual", RESIDUAL_MM_S2),
        "flag_spans": flag_spans(crashes),
        "quiet": bool(preset.get("quiet", False)),
        "slow_hz": SLOW_SIGNAL_HZ,
        "start_speed_mm_s": 0.0,
    }
    validate(cfg)
    return cfg


def validate(cfg):
    """Reject the configurations that would not produce a usable file."""
    if cfg["duration_ms"] <= 0:
        raise SystemExit("error: --duration-ms must be positive")
    if cfg["rate_hz"] != BASE_RATE_HZ:
        raise SystemExit("error: --rate-hz must be %d (one line per millisecond)"
                         % BASE_RATE_HZ)
    for crash in cfg["crashes"]:
        if crash["at"] < 0 or crash["at"] + crash["pulse_ms"] > cfg["duration_ms"]:
            raise SystemExit("error: crash at %d ms does not fit in %d ms"
                             % (crash["at"], cfg["duration_ms"]))
        peak = max(pulse_steps(crash["dv_kmh"], crash["pulse_ms"], crash["shape"]))
        if peak * STEP_TO_MM_S2 > MAX_PEAK_MM_S2:
            raise SystemExit("error: impact peak %d mm/s2 is out of range"
                             % (peak * STEP_TO_MM_S2))


def list_presets():
    """Print the preset catalogue with one description line each."""
    for name in PRESETS:
        print("%-18s %s" % (name, PRESETS[name]["description"]))


def parse_args(argv):
    """Build the argument parser."""
    parser = argparse.ArgumentParser(
        description="Generate an Ambre scenario file (docs/FORMAT.md section 11).")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--duration-ms", type=int, default=None)
    parser.add_argument("--crash-at-ms", type=int, default=None,
                        help="0 disables the crash entirely")
    parser.add_argument("--out", default=None, help="output scenario file")
    parser.add_argument("--dv-kmh", type=float, default=None)
    parser.add_argument("--rate-hz", type=int, default=BASE_RATE_HZ)
    parser.add_argument("--profile", choices=sorted(PROFILES), default="eu")
    parser.add_argument("--preset", choices=sorted(PRESETS), default=None)
    parser.add_argument("--list-presets", action="store_true")
    return parser.parse_args(argv)


def main(argv=None):
    """Entry point: list the presets or write one scenario file."""
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.list_presets:
        list_presets()
        return 0
    if not args.out:
        raise SystemExit("error: --out is required")
    cfg = build_config(args)
    rng = random.Random(args.seed)
    cfg["start_speed_mm_s"] = float(rng.randint(7000, 14_000))
    rows = simulate(cfg, rng)
    with open(args.out, "w", encoding="ascii", newline="\n") as handle:
        handle.write(render(cfg, rows))
    print("%s: %d samples, seed %d, %d impact(s)"
          % (args.out, len(rows), args.seed, len(cfg["crashes"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
