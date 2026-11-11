#!/usr/bin/env python3
"""Process-cut campaign for the Ambre EDR — the PC equivalent of level 2.

    python3 host/processcut/campaign.py --bins build --scenario scen.txt \
            --cycles 200 [--sweep-from-ms -50 --sweep-to-ms 500 --step-ms 1] \
            [--random-cuts N] [--jobs N] [--report out.json] [--resume]

Each cycle starts the three real processes (edr-journal, edr-capture,
edr-replay), plays the scenario, kills edr-journal with SIGKILL at the
chosen instant, restarts it on the SAME disk file so the recovery runs,
reads the recovered state, exports the event to MCAP and compares the
class A data with what was injected.

HONESTY -- READ THIS BEFORE QUOTING ANY NUMBER
----------------------------------------------
This campaign runs on the POSIX backend: the "raw partition" is a regular
file (pwrite + fdatasync) and the "power cut" is a SIGKILL on one process.
It is NOT a QEMU power cut and NOT a QNX target. Killing a process does not
lose the writes already handed to the host kernel, so this level exercises
the ORDER and the INTERRUPTION of the writes and the whole recovery path --
it cannot exercise the loss of a volatile cache. That stronger proof is the
level-1 exhaustive exploration (host/explore/, 64 591 cut states), and the
real power-cut campaign needs the QNX image under QEMU (spec 7.4). Every
published number must carry backend=posix.

English only, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for sub in (("host", "e2e"), ("host", "powercut")):
    path = os.path.join(ROOT, *sub)
    if path not in sys.path:
        sys.path.insert(0, path)

import e2e_cycle                                       # noqa: E402
import e2e_env                                         # noqa: E402
from campaign_report import (BACKEND, CUT_KIND, HONESTY, exit_code,  # noqa: E402
                             print_summary, summarise, write_reports)

NAMED_WINDOWS = ("pre", "acquired", "sealed", "recovery")
RANDOM_SPAN_MS = (-500, 800)


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Ambre process-cut campaign (level 2 on the PC backend)")
    p.add_argument("--bins", default="build", help="directory of the binaries")
    p.add_argument("--scenario", required=True, help="scenario file to inject")
    p.add_argument("--cycles", type=int, default=200, help="number of cuts")
    p.add_argument("--sweep-from-ms", type=int, default=None,
                   help="first sweep offset, in ms relative to the impact")
    p.add_argument("--sweep-to-ms", type=int, default=None)
    p.add_argument("--step-ms", type=int, default=1)
    p.add_argument("--random-cuts", type=int, default=0)
    p.add_argument("--jobs", type=int, default=1, help="parallel workers")
    p.add_argument("--report", default=None, help="JSON report (default: "
                   "<bins>/processcut-report.json)")
    p.add_argument("--state", default=None, help="JSONL state file (--resume)")
    p.add_argument("--resume", action="store_true", help="skip finished cycles")
    p.add_argument("--violation-dir", default=None,
                   help="where a violating cycle keeps its disk image")
    p.add_argument("--seed", type=int, default=20261111)
    p.add_argument("--rate", type=float, default=1.0, help="replay rate")
    p.add_argument("--no-verify", action="store_true",
                   help="skip the MCAP export and verification (faster)")
    p.add_argument("--quiet", action="store_true")
    return p.parse_args(argv)


def build_plan(args, crash_ms: int):
    """Deterministic list of cut points: sweep, then random, then a rotation
    of the four specification windows filled with numeric instants."""
    cuts = []
    if args.sweep_from_ms is not None and args.sweep_to_ms is not None:
        for off in range(args.sweep_from_ms, args.sweep_to_ms + 1, args.step_ms):
            cuts.append({"cut": crash_ms + off, "kind": "sweep%+d" % off})
    rng = random.Random(args.seed)
    low, high = RANDOM_SPAN_MS
    for _ in range(max(0, args.random_cuts)):
        cuts.append({"cut": "random", "kind": "random"})
    step = 0
    while len(cuts) < args.cycles:
        if step % 2 == 0:
            cuts.append({"cut": NAMED_WINDOWS[(step // 2) % 4],
                         "kind": NAMED_WINDOWS[(step // 2) % 4]})
        else:
            cuts.append({"cut": rng.randint(crash_ms + low, crash_ms + high),
                         "kind": "numeric"})
        step += 1
    return cuts[:max(0, args.cycles)]


class StateStore:
    """Append-only JSONL state: which cycles are already done (--resume)."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.done = {}
        if path and os.path.exists(path):
            with open(path) as handle:
                for line in handle:
                    try:
                        record = json.loads(line)
                    except ValueError:
                        continue
                    self.done[record.get("cycle")] = record

    def has(self, cycle) -> bool:
        return cycle in self.done

    def append(self, record) -> None:
        with self.lock:
            self.done[record["cycle"]] = record
            if self.path:
                with open(self.path, "a") as handle:
                    handle.write(json.dumps(record, sort_keys=True) + "\n")

    def records(self):
        return [self.done[key] for key in sorted(self.done)]


def record_of(result: dict, item: dict, cycle: int) -> dict:
    """Compact, resumable record of one cycle."""
    timeline = result.get("timeline", {})
    primary = e2e_cycle.primary_event(result.get("events", []))
    checks = result.get("checks", {})
    return {"cycle": cycle, "cut": item["cut"], "kind": item["kind"],
            "verdict": result.get("verdict"),
            "window": result.get("window"),
            "violations": result.get("violations", []),
            "error": result.get("error"),
            "events": len(result.get("events", [])),
            "primary": primary,
            "kill_at_ms": result.get("kill_at_ms"),
            "freeze_us": timeline.get("freeze_us"),
            "cycle_s": result.get("cycle_s"),
            "replay_rc": result.get("replay_rc"),
            "recovery_cut": result.get("recovery_cut"),
            "fidelity": {key: checks.get("fidelity", {}).get(key)
                         for key in ("ok", "matched", "total",
                                     "matched_exact_ms", "outside_scenario")},
            "official": checks.get("official", {})}


def run_one(args, item, cycle):
    """Run one cycle of the campaign and return its record."""
    artifact = os.path.join(args.violation_dir, "cycle-%04d" % cycle)
    started = time.monotonic()
    result = e2e_cycle.run_cycle(
        args.bins, args.scenario, item["cut"], rate=args.rate,
        verify=not args.no_verify, seed=args.seed + cycle,
        artifact_dir=artifact)
    record = record_of(result, item, cycle)
    record["wall_s"] = round(time.monotonic() - started, 3)
    return record
