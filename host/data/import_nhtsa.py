#!/usr/bin/env python3
"""NHTSA crash-test pulse -> Ambre scenario file (cahier §8, §3.3).

The third data source of the demonstration is "real crash pulses from the
public NHTSA crash-test database" (cahier §8). That database is not vendored
here and **the format of its data files is still to be confirmed** (cahier
§14, last row of the "verified or not" table). This importer therefore works on
a **flattened CSV extract** of one test and documents the correspondence, so a
genuine reader can be dropped in without touching anything else.

Correspondence with the real format
-----------------------------------
A NHTSA crash-test publication ships the sensor channels of one test (a
vehicle, a barrier or a moving deformable barrier, a set of accelerometers) as
tabular files, plus a test report. The channels this importer needs are the
vehicle-level accelerations, filtered to the SAE J211 class the EDR cares
about::

    "Vehicle Longitudinal Acceleration"   time series, in g
    "Vehicle Lateral Acceleration"        time series, in g
    "Vehicle Velocity Change"             integrated delta-v, in km/h

Real channels are sampled at 10 kHz and the time base is relative to the
impact. The fixture ``fixtures/nhtsa_pulse_sample.csv`` is such an extract
already decimated to 1 ms, which is the grid of docs/FORMAT.md §11:

    t_ms           time from impact, milliseconds
    accel_long_g   longitudinal acceleration, g   (-> mm/s^2)
    accel_lat_g    lateral acceleration, g        (-> mm/s^2)
    velocity_kmh   delta-v actually reached       (sanity check only)

Why the recorded delta-v is not written into the file
-----------------------------------------------------
A scenario carries accelerations, not delta-v: the trigger engine integrates
them itself (contract A3 of fmt/ambre_trigger.h). Writing the recorded delta-v
would give two sources for the same quantity, and the verifier could not tell a
disagreement from a rounding difference. The importer therefore checks that its
own integration closes with the recorded ``velocity_kmh`` within a documented
tolerance and fails loudly when it does not — that check is what proves the
unit conversion is right.

Usage::

    python3 host/data/import_nhtsa.py \\
        --fixture host/data/fixtures/nhtsa_pulse_sample.csv \\
        --out /tmp/nhtsa-scenario.txt [--duration-ms 3000] [--pre-ms 1000]

The scenario is built as: ``--pre-ms`` of steady approach at the speed the test
started from, then the recorded pulse, then coasting up to ``--duration-ms``.

No network access, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import List, Dict

sys.path.insert(0, str(Path(__file__).resolve().parent))

import synthetic  # noqa: E402  (thin layer over host/scenarios/generate.py)

DATA_DIR = Path(__file__).resolve().parent
DEFAULT_FIXTURE = DATA_DIR / "fixtures" / "nhtsa_pulse_sample.csv"

REQUIRED_COLUMNS = ("t_ms", "accel_long_g", "accel_lat_g")
FORBIDDEN_COLUMNS = ("vin", "lat", "lon", "gps", "date", "time", "driver",
                     "test_id", "photo")

# Unit conversions, stated instead of guessed.
G_TO_MM_S2 = 9806.65        # standard gravity, mm/s^2
KMH_TO_MM_S = 1000.0 / 3.6  # 1 km/h in mm/s
MS_TO_KMH = 3.6
MPS_TO_KMH_X4 = 14.4        # 1 m/s in 0.25 km/h units

# Closure tolerance of the integration check: the fixture is rounded to 0.1 g
# at 1 ms, so one sample can carry 0.05 g = 491 mm/s of error; over ~40 active
# samples that is well under 1 % of a 31 km/h pulse. 2 % leaves room without
# hiding a wrong constant (a g/km/h mix-up is off by 3.6 x).
INTEGRATION_TOLERANCE = 0.02


class ImportError_(SystemExit):
    """Raised on a malformed extract (exit status 2, like the other tools)."""


def read_pulse(path: str) -> List[Dict[str, str]]:
    """Read the CSV extract, skipping '#' comments; validate the columns."""
    rows: List[Dict[str, str]] = []
    with open(path, "r", encoding="utf-8", newline="") as handle:
        header: List[str] = []
        for raw in handle:
            if raw.startswith("#") or not raw.strip():
                continue
            if not header:
                header = [item.strip() for item in raw.rstrip("\n").split(",")]
                missing = [c for c in REQUIRED_COLUMNS if c not in header]
                if missing:
                    raise ImportError_("error: %s: missing column(s) %s"
                                       % (path, ", ".join(missing)))
                continue
            fields = raw.rstrip("\n").split(",")
            rows.append(dict(zip(header, fields)))
    if not rows:
        raise ImportError_("error: %s: no data row" % path)
    return rows


def forbidden_columns(path: str) -> List[str]:
    """Columns of the extract that VIE-01 forbids."""
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            if raw.startswith("#") or not raw.strip():
                continue
            header = [item.strip() for item in raw.rstrip("\n").split(",")]
            return [c for c in header if c in FORBIDDEN_COLUMNS]
    return []


def pulse_rows(path: str) -> List[tuple]:
    """(t_ms, a_long_mm_s2, a_lat_mm_s2) of the recorded pulse."""
    out: List[tuple] = []
    for row in read_pulse(path):
        out.append((int(row["t_ms"]),
                    int(round(float(row["accel_long_g"]) * G_TO_MM_S2)),
                    int(round(float(row["accel_lat_g"]) * G_TO_MM_S2))))
    return out


def integrated_kmh(rows: List[tuple]) -> float:
    """Integrate the longitudinal acceleration with the engine's own rule.

    Same rectangle rule as contract A3 (fmt/ambre_trigger.h): per sample, the
    current acceleration times the elapsed time. That is what the trigger
    engine will compute, so the check below compares like with like.
    """
    total = 0.0
    for index in range(1, len(rows)):
        dt_s = (rows[index][0] - rows[index - 1][0]) / 1000.0
        total += rows[index][1] * dt_s
    return abs(total) / KMH_TO_MM_S


def recorded_kmh(path: str) -> float:
    """Delta-v the extract claims, in km/h (max of the velocity column)."""
    values = [abs(float(row["velocity_kmh"])) for row in read_pulse(path)]
    return max(values) if values else 0.0


def check_integration(path: str) -> tuple:
    """(integrated, recorded, relative error); raises when they disagree."""
    rows = pulse_rows(path)
    integrated = integrated_kmh(rows)
    recorded = recorded_kmh(path)
    if recorded <= 0.0:
        return integrated, recorded, 0.0
    error = abs(integrated - recorded) / recorded
    if error > INTEGRATION_TOLERANCE:
        raise ImportError_(
            "error: %s: integrated delta-v %.2f km/h disagrees with the "
            "recorded %.2f km/h (%.1f %% > %.0f %%)"
            % (path, integrated, recorded, error * 100.0,
               INTEGRATION_TOLERANCE * 100.0))
    return integrated, recorded, error


def approach_samples(count: int, speed_kmh_x4: int) -> List[synthetic.Sample]:
    """Steady cruise before the impact: no acceleration, constant speed."""
    return [synthetic.Sample(speed_kmh_x4=speed_kmh_x4) for _ in range(count)]


def coast_samples(count: int, speed_kmh_x4: int) -> List[synthetic.Sample]:
    """After the pulse: the vehicle has lost its delta-v and keeps rolling."""
    return [synthetic.Sample(speed_kmh_x4=speed_kmh_x4) for _ in range(count)]


def build_samples(path: str, pre_ms: int, duration_ms: int) -> List[synthetic.Sample]:
    """Approach + recorded pulse + coast, on the 1 ms grid."""
    rows = pulse_rows(path)
    if len(rows) > duration_ms:
        raise ImportError_(
            "error: the pulse (%d ms) does not fit in --duration-ms %d"
            % (len(rows), duration_ms))
    # The test starts at a standstill of the barrier setup: the cruise speed is
    # chosen so the recorded delta-v is plausible, and it is a round number of
    # the 0.25 km/h grid rather than a measured value (this extract carries no
    # speed channel).
    cruise_x4 = int(round(50 * 4))          # 50 km/h, in 0.25 km/h units
    samples = approach_samples(pre_ms, cruise_x4)
    for _, a_long, a_lat in rows:
        samples.append(synthetic.Sample(a_long_mm_s2=a_long,
                                        a_lat_mm_s2=a_lat,
                                        speed_kmh_x4=cruise_x4,
                                        brake=1 if a_long < 0 else 0))
    remaining = duration_ms - len(samples)
    samples.extend(coast_samples(max(0, remaining), cruise_x4))
    return samples


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Import a NHTSA crash-test pulse as an Ambre scenario")
    parser.add_argument("--fixture", default=str(DEFAULT_FIXTURE),
                        help="flattened CSV extract of one test")
    parser.add_argument("--out", required=True)
    parser.add_argument("--pre-ms", type=int, default=1000,
                        help="steady approach before the impact")
    parser.add_argument("--duration-ms", type=int, default=3000)
    parser.add_argument("--name", default="nhtsa-pulse")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    integrated, recorded, error = check_integration(args.fixture)
    samples = build_samples(args.fixture, args.pre_ms, args.duration_ms)
    crash_at = args.pre_ms
    written = synthetic.write_scenario(args.out, args.name, 0, samples, crash_at)
    dropped = forbidden_columns(args.fixture)
    print("%s: %d samples (%.1f s at 1 kHz), impact at %d ms"
          % (args.out, written, written / 1000.0, crash_at))
    print("pulse: integrated %.2f km/h, recorded %.2f km/h (%.2f %% apart)"
          % (integrated, recorded, error * 100.0))
    if dropped:
        print("privacy: dropped %s at import (VIE-01)" % ", ".join(dropped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
