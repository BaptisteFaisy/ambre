#!/usr/bin/env python3
"""comma2k19 recording -> Ambre scenario file (cahier §8, §3.3).

comma2k19 is a real open driving dataset. Its real form is **one HDF5 file per
~1-minute segment**, and reading it needs ``h5py``, which this task forbids.
This importer therefore works on a **flattened CSV extract** of one segment and
documents the exact correspondence, so swapping in a true HDF5 reader later is
a change of one function (``read_extract``) and nothing else.

Correspondence with the real format
-----------------------------------
The published segment layout (comma2k19 ``README``) is::

    <route>/<segment>/video.hevc
    <route>/<segment>/processed_log/CAN/<n>/speed        float32, m/s
    <route>/<segment>/processed_log/GPS/<n>/[lat,lon,alt,speed_x,speed_y,vN,vE]
    <route>/<segment>/processed_log/IMU/<n>/[accel_x,accel_y,accel_z,gyro_x,gyro_y]
    <route>/<route>.txt                                  pose[3] per frame
    <route>/global_pose/frame_positions, frame_orientations

Each ``<n>`` is a message index, and each series carries its own time base: the
CAN log at 100 Hz, the IMU at 100 Hz, the GNSS as it arrives. A real importer
must (1) read those series, (2) align them on a common relative time base, and
(3) decimate to the 1 ms grid of docs/FORMAT.md §11.

The fixture used here, ``fixtures/comma2k19_segment_sample.csv``, is that step
(1) and (2) already done, with one row per 100 Hz tick and these columns:

    t_ms              relative time from the start of the segment
    pos_x,pos_y,pos_z pose[3], metres      (DISCARDED: position, VIE-01)
    lat,lon           degrees              (DISCARDED: position, VIE-01)
    speed             m/s                  (kept: becomes speed_kmh_x4)
    accel_x,accel_y   m/s^2                (kept: become mm/s^2 on both axes)
    accel_z           m/s^2                (not a class A field, dropped)
    gyro_x,gyro_y     rad/s                (dropped: not a class A field)
    wheel_speed_mps   m/s                  (kept: sanity check only)

Privacy (VIE-01) is decided **here, once**: position, GNSS and every absolute
time are dropped at import, so they cannot reach a scenario file. The absolute
time base never enters the pipeline: only ``t_ms`` relative to the segment
start is used, and it is rebased to 0 on output.

Usage::

    python3 host/data/import_comma2k19.py \\
        --fixture host/data/fixtures/comma2k19_segment_sample.csv \\
        --out /tmp/comma2k19-scenario.txt \\
        [--crash-at-ms N] [--dv-kmh X]

Without ``--crash-at-ms`` the recording is imported as it is (a plain drive).
With it, a crash pulse is spliced in at that instant, using the generator's
pulse model through ``host/data/synthetic.py``.

No network access, Python 3 standard library only (no h5py, no numpy).
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Dict, List

sys.path.insert(0, str(Path(__file__).resolve().parent))

import synthetic  # noqa: E402  (thin layer over host/scenarios/generate.py)

DATA_DIR = Path(__file__).resolve().parent
DEFAULT_FIXTURE = DATA_DIR / "fixtures" / "comma2k19_segment_sample.csv"

# Real segment series are 100 Hz (comma2k19 README, CAN and IMU logs).
SOURCE_PERIOD_MS = 100

# Columns the importer needs, and the ones it must never carry through.
REQUIRED_COLUMNS = ("t_ms", "speed", "accel_x", "accel_y")
FORBIDDEN_COLUMNS = ("pos_x", "pos_y", "pos_z", "lat", "lon", "alt",
                     "vin", "date", "time", "driver")

# Unit conversions (documented, not guessed).
MPS2_TO_MM_S2 = 1000.0      # m/s^2 -> mm/s^2
KMH_X4_FROM_MPS = 14.4      # 1 m/s = 3.6 km/h, stored in 0.25 km/h units


class ImportError_(SystemExit):
    """Raised on a malformed extract (exit status 2, like the other tools)."""


def read_extract(path: str) -> List[Dict[str, str]]:
    """Read the flattened CSV extract, skipping '#' comments.

    This is the single function to replace with an ``h5py`` reader to consume
    the genuine comma2k19 segments: it must return the same column names.
    """
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
                    raise ImportError_(
                        "error: %s: missing column(s) %s"
                        % (path, ", ".join(missing)))
                continue
            fields = raw.rstrip("\n").split(",")
            rows.append(dict(zip(header, fields)))
    if not rows:
        raise ImportError_("error: %s: no data row" % path)
    return rows


def forbidden_columns(path: str) -> List[str]:
    """Columns of the extract that VIE-01 forbids (should be the GNSS ones)."""
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            if raw.startswith("#") or not raw.strip():
                continue
            header = [item.strip() for item in raw.rstrip("\n").split(",")]
            return [c for c in header if c in FORBIDDEN_COLUMNS]
    return []


def to_sample(row: Dict[str, str]) -> synthetic.Sample:
    """One 100 Hz extract row -> one scenario sample.

    Only the three class A quantities the real CAN/IMU logs carry are kept:
    longitudinal acceleration (``accel_x``), lateral acceleration (``accel_y``)
    and the speed. The engine speed, pedals and steering angle are **not** in
    this extract, so they stay at their zero value: they are not invented.
    """
    speed_mps = float(row["speed"])
    speed_x4 = int(round(speed_mps * KMH_X4_FROM_MPS))
    return synthetic.Sample(
        a_long_mm_s2=int(round(float(row["accel_x"]) * MPS2_TO_MM_S2)),
        a_lat_mm_s2=int(round(float(row["accel_y"]) * MPS2_TO_MM_S2)),
        speed_kmh_x4=max(0, min(speed_x4, 65535)),
        throttle=0,
        brake=0,
        rpm=0,
        steer_x2=0,
        ext_flags=0,
    )


def build_samples(path: str) -> List[synthetic.Sample]:
    """Extract -> 100 Hz samples -> 1 kHz scenario grid (zero-order hold)."""
    rows = read_extract(path)
    coarse = [to_sample(row) for row in rows]
    return synthetic.resample(coarse, SOURCE_PERIOD_MS)


def crash_at_ms_for(samples: List[synthetic.Sample], requested: int) -> int:
    """Where the impact is spliced; 0 means "no impact".

    A request beyond the recording is refused rather than silently clamped:
    clamping would produce a file whose ``crash_at_ms`` header lies.
    """
    if requested <= 0:
        return 0
    if requested >= len(samples):
        raise ImportError_(
            "error: --crash-at-ms %d is past the recording (%d ms)"
            % (requested, len(samples)))
    return requested


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Import a comma2k19 segment extract as an Ambre scenario")
    parser.add_argument("--fixture", default=str(DEFAULT_FIXTURE),
                        help="flattened CSV extract of one segment")
    parser.add_argument("--out", required=True)
    parser.add_argument("--crash-at-ms", type=int, default=0,
                        help="splice a crash pulse at this millisecond")
    parser.add_argument("--dv-kmh", type=float, default=12.0,
                        help="delta-v of the spliced pulse")
    parser.add_argument("--pulse-ms", type=int, default=120)
    parser.add_argument("--shape", default="crash",
                        choices=("crash", "flat", "ramp", "sine", "late"))
    parser.add_argument("--name", default="comma2k19-segment")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    samples = build_samples(args.fixture)
    crash_at = crash_at_ms_for(samples, args.crash_at_ms)
    if crash_at > 0:
        pulse = synthetic.pulse_samples(args.dv_kmh, args.pulse_ms, args.shape)
        synthetic.inject_pulse(samples, pulse, crash_at)
    written = synthetic.write_scenario(args.out, args.name, 0, samples, crash_at)
    dropped = forbidden_columns(args.fixture)
    print("%s: %d samples (%.1f s at 1 kHz), crash_at_ms=%d"
          % (args.out, written, written / 1000.0, crash_at))
    if dropped:
        print("privacy: dropped %s at import (VIE-01)" % ", ".join(dropped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
