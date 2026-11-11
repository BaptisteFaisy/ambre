#!/usr/bin/env python3
"""Multi-shock scenario generator for the latency campaign.

Emits a docs/FORMAT.md section 11 scenario holding N shock pulses spaced by a
fixed cycle, so a latency chunk (one shock) can be cut out of it and replayed
at real time.  Design constraints (fmt/ambre_trigger.h):

  * trigger  : 150 ms window excursion > 8 km/h   (AMBRE_DV_TRIGGER_MM_S)
  * lock     : 150 ms window excursion > 25 km/h  (AMBRE_DV_LOCK_MM_S)
  * event end: 20 ms window excursion <= 0.8 km/h (spec 2.7)

The pulse is a flat deceleration of PEAK_MM_S2 (-250 000 mm/s^2, about
25.5 g): every sample is a multiple of 1000 mm/s^2, like the fixed generator
host/scenarios/generate.py, so the integer rectangle rule of the trigger
engine integrates it exactly.  Its length is derived from the requested
delta-v, which stays above the trigger threshold and far below the lock
threshold, so every event stays unlocked.

Usage:
    python3 host/latency/multi_shock.py --triggers 30 --out /tmp/sc.txt
    python3 host/latency/multi_shock.py --triggers 3 --cycle-ms 400 \
        --dv-kmh 9.0,10.8 --out /tmp/quick.txt
"""

from __future__ import annotations

import argparse
import random
import sys

RATE_HZ = 1000                 # FORMAT.md section 11: one row per millisecond
PEAK_MM_S2 = -250000           # about 25.5 g, multiple of 1000
MM_S_PER_PULSE_MS = 250        # one millisecond at PEAK_MM_S2, in mm/s
DEFAULT_DV_KMH = (10.8,)       # above 8 (trigger), below 25 (lock)
UM_S_PER_KMH_X4 = 277778 // 4  # 1 unit of speed_kmh_x4 in micrometres/second
START_SPEED_KMH_X4 = 200       # 50 km/h

COLUMNS = ("# t_us a_long_mm_s2 a_lat_mm_s2 speed_kmh_x4 throttle brake "
           "rpm steer_x2 ext_flags")


def pulse_length_ms(dv_kmh):
    """Milliseconds of PEAK_MM_S2 needed for `dv_kmh` of delta-v."""
    dv_mm_s = int(round(abs(dv_kmh) * 1000.0 / 3.6))
    return max(1, -(-dv_mm_s // MM_S_PER_PULSE_MS))


def row_text(t_ms, a_long, a_lat, speed_x4):
    """One scenario row; ext_flags stays 0 so no event is ever locked."""
    return f"{t_ms * 1000} {a_long} {a_lat} {speed_x4} 0 0 800 0 0\n"


def shock_marks(triggers, cycle_ms, lead_ms, dv_list):
    """{row index: acceleration} of every pulse sample of the scenario."""
    marks = {}
    for index in range(triggers):
        dv = dv_list[index % len(dv_list)]
        start = lead_ms + index * cycle_ms
        for offset in range(pulse_length_ms(dv)):
            marks[start + offset] = PEAK_MM_S2
    return marks


def build_rows(triggers, cycle_ms, lead_ms, tail_ms, seed, dv_list):
    """Every row of the scenario, as text lines, speeds integrated exactly."""
    rng = random.Random(seed)
    total_ms = lead_ms + triggers * cycle_ms + tail_ms
    marks = shock_marks(triggers, cycle_ms, lead_ms, dv_list)
    speed_um_s = START_SPEED_KMH_X4 * UM_S_PER_KMH_X4
    lines = []
    for t_ms in range(total_ms):
        if t_ms in marks:
            a_long, a_lat = marks[t_ms], 0
        else:
            a_long, a_lat = rng.randint(-20, 20), rng.randint(-30, 30)
        speed_um_s = max(0, speed_um_s + a_long)
        speed_x4 = min(65535, (speed_um_s * 4) // 277778)
        lines.append(row_text(t_ms, a_long, a_lat, speed_x4))
    return lines, total_ms


def scenario_text(args):
    """Full scenario file content (header + columns + rows)."""
    lines, total_ms = build_rows(args.triggers, args.cycle_ms, args.lead_ms,
                                 args.tail_ms, args.seed, args.dv_values)
    head = [
        "# ambre scenario v1\n",
        f"name=multi-shock-{args.triggers}\n",
        f"seed={args.seed}\n",
        f"rate_hz={RATE_HZ}\n",
        f"duration_ms={total_ms}\n",
        f"crash_at_ms={args.lead_ms}\n",
        COLUMNS + "\n",
    ]
    return "".join(head + lines)


def parse_dv_list(text):
    values = [float(item) for item in text.split(",") if item.strip()]
    if not values:
        raise argparse.ArgumentTypeError("--dv-kmh needs at least one value")
    for value in values:
        if not 8.1 <= value <= 24.0:
            raise argparse.ArgumentTypeError(
                f"--dv-kmh {value} out of range: it must trigger (> 8 km/h) "
                "and stay unlocked (< 25 km/h)")
    return values


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--triggers", type=int, required=True,
                   help="number of shock pulses")
    p.add_argument("--cycle-ms", type=int, default=1000,
                   help="spacing between two shocks (default 1000)")
    p.add_argument("--lead-ms", type=int, default=400,
                   help="quiet run-up before the first shock (default 400)")
    p.add_argument("--tail-ms", type=int, default=400,
                   help="quiet tail after the last shock (default 400)")
    p.add_argument("--dv-kmh", type=parse_dv_list, default=list(DEFAULT_DV_KMH),
                   help="delta-v per shock, comma separated, cycled "
                        "(default 10.8)")
    p.add_argument("--seed", type=int, default=20261111)
    p.add_argument("--out", required=True, help="scenario file to write")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    args.dv_values = args.dv_kmh
    if args.triggers < 1 or args.cycle_ms < 100:
        print("need --triggers >= 1 and --cycle-ms >= 100", file=sys.stderr)
        return 2
    rows = args.triggers * args.cycle_ms + args.lead_ms + args.tail_ms
    if rows > 120000:
        print(f"scenario would hold {rows} rows, above the 120000 limit of "
              "scenario_file.h; lower --triggers or --cycle-ms",
              file=sys.stderr)
        return 2
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(scenario_text(args))
    print(f"{args.out}: {args.triggers} shocks, {rows} rows, "
          f"{rows / 1000.0:.1f} s at rate 1.0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
