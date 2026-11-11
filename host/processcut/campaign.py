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
from campaign_report import (exit_code, print_summary, summarise,  # noqa: E402
                             write_reports)

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
            "edrctl_used": (result.get("edrctl") or {}).get("used"),
            "recovery_cut": result.get("recovery_cut"),
            "fidelity": {key: checks.get("fidelity", {}).get(key)
                         for key in ("ok", "ok_strict", "matched", "total",
                                     "matched_exact_ms", "outside_scenario",
                                     "worst_row_slop_ms", "skew_ms",
                                     "problems")},
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


def prepare_paths(args):
    """Resolve the report, state and violation paths; start clean unless
    --resume was asked."""
    report = args.report or os.path.join(args.bins, "processcut-report.json")
    args.report = os.path.abspath(report)
    parent = os.path.dirname(args.report)
    if parent:
        os.makedirs(parent, exist_ok=True)
    args.state = os.path.abspath(args.state or (args.report + ".state.jsonl"))
    args.violation_dir = os.path.abspath(
        args.violation_dir or (args.report + "-violations"))
    if not args.resume:
        with open(args.state, "w"):
            pass


def worker(args, item, cycle, state, lock, counter):
    """One cycle, then its record in the state file and one progress line."""
    record = run_one(args, item, cycle)
    state.append(record)
    with lock:
        counter[0] += 1
        if not args.quiet:
            print("[%3d/%3d] cycle %3d %-9s window=%-18s verdict=%-17s "
                  "events=%d" % (counter[0], counter[1], cycle,
                                 str(record.get("kind"))[:9],
                                 record.get("window"), record.get("verdict"),
                                 record.get("events", 0)), flush=True)
    return record


def run_plan(args, plan, state):
    """Execute every pending cycle, serially or with a pool of workers."""
    todo = [(index, item) for index, item in enumerate(plan)
            if not state.has(index)]
    lock = threading.Lock()
    counter = [0, len(todo)]
    started = time.monotonic()
    if args.jobs <= 1:
        for index, item in todo:
            worker(args, item, index, state, lock, counter)
    else:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(worker, args, item, index, state, lock,
                                   counter) for index, item in todo]
            for future in futures:
                future.result()
    return time.monotonic() - started


def main(argv=None) -> int:
    args = parse_args(argv)
    prepare_paths(args)
    if not os.path.isfile(args.scenario):
        raise SystemExit("error: scenario not found: %s" % args.scenario)
    try:
        e2e_env.resolve_binaries(args.bins)
    except e2e_env.BenchError as exc:
        raise SystemExit("error: %s" % exc)
    crash_ms = int(e2e_cycle.scenario_header(args.scenario)
                   .get("crash_at_ms", 0))
    plan = build_plan(args, crash_ms)
    state = StateStore(args.state)
    elapsed = run_plan(args, plan, state)
    records = state.records()
    summary = summarise(records)
    write_reports(args, records, summary, plan)
    print_summary(args, records, summary, elapsed)
    return exit_code(summary)


if __name__ == "__main__":
    sys.exit(main())
