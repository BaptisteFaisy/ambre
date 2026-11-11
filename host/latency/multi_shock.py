#!/usr/bin/env python3
"""Multi-shock scenario generator for the latency campaign.

Emits a docs/FORMAT.md section 11 scenario holding N shock pulses spaced by a
fixed cycle, so one edr-replay run produces N successive triggers without any
restart.  Design constraints (fmt/ambre_trigger.h):

  * trigger  : window delta-v > 8 km/h within 150 ms   (AMBRE_DV_TRIGGER_MM_S)
  * lock     : window delta-v > 25 km/h within 150 ms  (AMBRE_DV_LOCK_MM_S)
  * event end: 20 ms cumulative delta-v <= 0.8 km/h    (spec 2.7)

The default pulse is 12 samples of -250 000 mm/s^2 (~25.5 g): the rectangle
rule gives exactly 3000 mm/s = 10.8 km/h of delta-v, above the trigger
threshold and safely below the lock threshold, so every event stays unlocked
and the journal may recycle its segments (STO-06) for arbitrarily many
triggers.  Impact samples are multiples of 1000 mm/s^2, like the fixed
generator host/scenarios/generate.py, so the integer integration is exact.

Usage:
    python3 host/latency/multi_shock.py --triggers 200 --out /tmp/sc.txt
    python3 host/latency/multi_shock.py --triggers 3 --cycle-ms 400 \
        --lead-ms 300 --tail-ms 200 --out /tmp/quick.txt
"""

from __future__ import annotations

import argparse
import random
import sys

RATE_HZ = 1000                 # FORMAT.md section 11: one row per millisecond
PULSE_ACC_MM_S2 = -250000      # ~25.5 g, multiple of 1000 -> exact rectangle
PULSE_MS = 12                  # 12 x 0.9 km/h = 10.8 km/h delta-v (no lock)
START_SPEED_KMH_X4 = 200       # 50 km/h
UM_S_PER_KMH_X4 = 277778 // 4  # 1 unit of speed_kmh_x4 in micrometres/second

COLUMNS = ("# t_us a_long_mm_s2 a_lat_mm_s2 speed_kmh_x4 throttle brake "
           "rpm steer_x2 ext_flags")


def row_text(t_ms, a_long, a_lat, speed_x4):
    """One scenario row; ext_flags stays 0 so no event is ever locked."""
    return f"{t_ms * 1000} {a_long} {a_lat} {speed_x4} 0 0 800 0 0\n"


def pulse_start_ms(index, lead_ms, cycle_ms):
    return lead_ms + index * cycle_ms


def build_rows(triggers, cycle_ms, lead_ms, tail_ms, seed):
    """Every row of the scenario, as text lines, speeds integrated exactly."""
    rng = random.Random(seed)
    total_ms = lead_ms + triggers * cycle_ms + tail_ms
    pulse_of = {pulse_start_ms(k, lead_ms, cycle_ms) + j: True
                for k in range(triggers) for j in range(PULSE_MS)}
    speed_um_s = START_SPEED_KMH_X4 * UM_S_PER_KMH_X4
    lines = []
    for t_ms in range(total_ms):
        if pulse_of.get(t_ms):
            a_long, a_lat = PULSE_ACC_MM_S2, 0
        else:
            a_long, a_lat = rng.randint(-20, 20), rng.randint(-30, 30)
        speed_um_s = max(0, speed_um_s + a_long)
        speed_x4 = min(65535, (speed_um_s * 4) // 277778)
        lines.append(row_text(t_ms, a_long, a_lat, speed_x4))
    return lines, total_ms


def scenario_text(args):
    """Full scenario file content (header + columns + rows)."""
    lines, total_ms = build_rows(args.triggers, args.cycle_ms, args.lead_ms,
                                 args.tail_ms, args.seed)
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


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--triggers", type=int, required=True,
                   help="number of shock pulses (one trigger each)")
    p.add_argument("--cycle-ms", type=int, default=500,
                   help="spacing between two shocks (default 500)")
    p.add_argument("--lead-ms", type=int, default=400,
                   help="quiet run-up before the first shock (default 400)")
    p.add_argument("--tail-ms", type=int, default=300,
                   help="quiet tail so the last event seals (default 300)")
    p.add_argument("--seed", type=int, default=20261111)
    p.add_argument("--out", required=True, help="scenario file to write")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
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
