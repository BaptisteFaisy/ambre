#!/usr/bin/env python3
"""Reporting half of the process-cut campaign (see campaign.py)."""

from __future__ import annotations

import json
import os

from vm_util import histogram_text, stats

VIOLATION_VERDICTS = ("unexpected_loss", "inconsistent_state")

BACKEND = "posix"
CUT_KIND = "SIGKILL on the edr-journal process"
HONESTY = (
    "backend=posix: the journal partition is a regular file and the cut is a "
    "SIGKILL on one process, NOT a QEMU/QNX power cut. Writes already handed "
    "to the host kernel survive the kill, so this campaign proves the order "
    "and the interruption of the writes plus the whole recovery path; it does "
    "NOT prove anything about a lost volatile cache (level 1, host/explore/) "
    "nor about the QNX target (spec 7.4, docs/QNX-PORT.md).")


def summarise(records):
    """Aggregate the verdicts, the cut windows and the timings."""
    verdicts, windows, kinds = {}, {}, {}
    freeze, cycles, kills = [], [], []
    violations = 0
    for record in records:
        verdict = record.get("verdict") or "error"
        verdicts[verdict] = verdicts.get(verdict, 0) + 1
        window = record.get("window") or "none"
        windows[window] = windows.get(window, 0) + 1
        kind = record.get("kind") or "?"
        kinds[kind] = kinds.get(kind, 0) + 1
        violations += len(record.get("violations") or [])
        if record.get("freeze_us"):
            freeze.append(record["freeze_us"] / 1000.0)
        if isinstance(record.get("cycle_s"), (int, float)):
            cycles.append(record["cycle_s"])
        if isinstance(record.get("kill_at_ms"), (int, float)):
            kills.append(record["kill_at_ms"] / 1000.0)
    return {"cuts": len(records), "verdicts": verdicts, "windows": windows,
            "kinds": kinds, "violations": violations,
            "errors": verdicts.get("error", 0),
            "freeze_ms": stats(freeze), "cycle_s": stats(cycles),
            "kill_at_s": stats(kills)}


def write_reports(args, records, summary, plan) -> str:
    """Write the JSON report; violations also keep their disk image."""
    report = {
        "tool": "processcut_campaign",
        "backend": BACKEND,
        "cut_kind": CUT_KIND,
        "honesty": HONESTY,
        "scenario": os.path.abspath(args.scenario),
        "bins": os.path.abspath(args.bins),
        "plan": {"cycles": args.cycles, "jobs": args.jobs, "seed": args.seed,
                 "rate": args.rate,
                 "sweep": {"from_ms": args.sweep_from_ms,
                           "to_ms": args.sweep_to_ms, "step_ms": args.step_ms},
                 "random_cuts": args.random_cuts,
                 "composition": {item["kind"]: sum(1 for p in plan
                                                   if p["kind"] == item["kind"])
                                 for item in plan}},
        "summary": summary,
        "runs": records,
    }
    with open(args.report, "w") as handle:
        json.dump(report, handle, indent=2, sort_keys=True, default=str)
        handle.write("\n")
    return args.report


def _print_counts(title, counts):
    parts = " ".join("%s=%d" % (key, counts[key]) for key in sorted(counts))
    print("  %-16s: %s" % (title, parts or "-"))


def print_summary(args, records, summary, elapsed_s):
    """Readable recap, with the honesty note and the histograms."""
    print("")
    print("process-cut campaign — backend=%s, cut=%s" % (BACKEND, CUT_KIND))
    print("  cycles executed : %d / %d requested (jobs=%d)"
          % (summary["cuts"], args.cycles, args.jobs))
    _print_counts("verdicts", summary["verdicts"])
    _print_counts("cut windows", summary["windows"])
    print("  violations      : %d" % summary["violations"])
    print("  errors          : %d" % summary["errors"])
    for record in records:
        if record.get("error"):
            print("    first error   : cycle %s: %s"
                  % (record["cycle"], record["error"]))
            break
    for record in records:
        for item in record.get("violations") or []:
            print("    violation     : cycle %s: %s" % (record["cycle"], item))
    freeze = summary["freeze_ms"]
    if freeze.get("count"):
        print("  freeze (ms)     : min %.3f  median %.3f  p99 %.3f  max %.3f"
              "  (n=%d)" % (freeze["min"], freeze["median"], freeze["p99"],
                            freeze["max"], freeze["count"]))
    cycle = summary["cycle_s"]
    if cycle.get("count"):
        print("  cycle time (s)  : min %.2f  median %.2f  p99 %.2f  max %.2f"
              % (cycle["min"], cycle["median"], cycle["p99"], cycle["max"]))
    if elapsed_s > 0 and summary["cuts"]:
        print("  throughput      : %.1f cuts/min (%.1f s wall, %.2f s/cut "
              "with %d job(s))" % (60.0 * summary["cuts"] / elapsed_s,
                                   elapsed_s, elapsed_s / summary["cuts"],
                                   args.jobs))
    kills = [record.get("kill_at_ms") for record in records
             if isinstance(record.get("kill_at_ms"), (int, float))]
    print(histogram_text([value / 1000.0 for value in kills],
                         "cut instant histogram (s after the replay started)"),
          end="")
    print(histogram_text([record["cycle_s"] for record in records
                          if isinstance(record.get("cycle_s"), (int, float))],
                         "cycle time histogram"), end="")
    print("  report          : %s" % args.report)
    print("  state           : %s" % args.state)
    if summary["violations"]:
        print("  violations kept : %s" % args.violation_dir)
    print("")
    print("  %s" % HONESTY)
    print("")


def exit_code(summary) -> int:
    """0 only when nothing was lost, nothing was inconsistent, nothing errored."""
    if summary["violations"] or summary["errors"]:
        return 1
    return 0
