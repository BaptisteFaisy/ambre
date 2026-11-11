#!/usr/bin/env python3
"""Reporting half of the process-cut campaign (see campaign.py).

Everything printed here comes from the recorded cycles: no estimate, no
default value passed off as a measurement. A field a cycle did not produce is
shown as "-".
"""

from __future__ import annotations

import json
import os

from vm_util import histogram_text, stats

ROW_SLOP_MS = 4      # documented row slop of the strict class A comparison

VIOLATION_VERDICTS = ("unexpected_loss", "inconsistent_state")
BAD_VERDICTS = VIOLATION_VERDICTS + ("check_failed", "error")

BACKEND = "posix"
CUT_KIND = "SIGKILL on the edr-journal process"
HONESTY = (
    "backend=posix: the journal partition is a regular file and the cut is a "
    "SIGKILL on one process, NOT a QEMU/QNX power cut. Writes already handed "
    "to the host kernel survive the kill, so this campaign proves the order "
    "and the interruption of the writes plus the whole recovery path; it does "
    "NOT prove anything about a lost volatile cache (level 1, host/explore/) "
    "nor about the QNX target (spec 7.4, docs/QNX-PORT.md).")


def _new_acc():
    return {"verdicts": {}, "windows": {}, "kinds": {}, "freeze": [],
            "cycles": [], "kills": [], "skews": [], "drifts": [],
            "violations": 0, "strict": 0, "checks_failed": 0,
            "check_names": set(), "upstream": 0, "upstream_names": set(),
            "attempts": 1, "retried": 0}


def _count_record(acc, record):
    """Fold one cycle record into the accumulator."""
    verdict = record.get("verdict") or "error"
    acc["verdicts"][verdict] = acc["verdicts"].get(verdict, 0) + 1
    window = record.get("window") or "none"
    acc["windows"][window] = acc["windows"].get(window, 0) + 1
    kind = record.get("kind") or "?"
    acc["kinds"][kind] = acc["kinds"].get(kind, 0) + 1
    acc["violations"] += len(record.get("violations") or [])
    if record.get("freeze_us"):
        acc["freeze"].append(record["freeze_us"] / 1000.0)
    for key, bucket, scale in (("cycle_s", "cycles", 1.0),
                               ("kill_at_ms", "kills", 0.001)):
        value = record.get(key)
        if isinstance(value, (int, float)):
            acc[bucket].append(value * scale)
    _count_fidelity(acc, record.get("fidelity") or {})
    _count_official(acc, record)
    if isinstance(record.get("attempts"), int):
        acc["attempts"] = max(acc["attempts"], record["attempts"])
        acc["retried"] += record["attempts"] > 1


def _count_fidelity(acc, fidelity):
    """Class A comparison counters: strict, harness lag, producer drift."""
    if fidelity.get("ok_strict"):
        acc["strict"] += 1
    if fidelity.get("skew_ms") is not None:
        acc["skews"].append(float(fidelity["skew_ms"]))
    if fidelity.get("worst_drift_ms") is not None:
        acc["drifts"].append(float(fidelity["worst_drift_ms"]))


def _count_official(acc, record):
    """Official verifier counters, upstream findings apart."""
    official = record.get("official") or {}
    for item in official.get("upstream") or []:
        acc["upstream"] += 1
        acc["upstream_names"].add("%s(%s)" % (
            item.get("check"), ",".join(item.get("topics", {}))))
    if record.get("verdict") != "check_failed":
        return
    acc["checks_failed"] += 1
    acc["check_names"].update(official.get("failed") or [])
    if not (record.get("fidelity") or {}).get("ok"):
        acc["check_names"].add("class_a.fidelity(bench)")


def summarise(records):
    """Aggregate the verdicts, the cut windows and the timings."""
    acc = _new_acc()
    for record in records:
        _count_record(acc, record)
    return {"cuts": len(records), "verdicts": acc["verdicts"],
            "windows": acc["windows"], "kinds": acc["kinds"],
            "violations": acc["violations"],
            "errors": acc["verdicts"].get("error", 0),
            "freeze_ms": stats(acc["freeze"]), "cycle_s": stats(acc["cycles"]),
            "kill_at_s": stats(acc["kills"]),
            "class_a_strict": acc["strict"],
            "harness_skew_ms": stats(acc["skews"]),
            "harness_drift_ms": stats(acc["drifts"]),
            "max_attempts": acc["attempts"], "cycles_retried": acc["retried"],
            "checks_failed": acc["checks_failed"],
            "check_names": sorted(acc["check_names"]),
            "upstream_findings": acc["upstream"],
            "upstream_checks": sorted(acc["upstream_names"])}


def write_reports(args, records, summary, plan) -> str:
    """Write the JSON report; violations also keep their disk image."""
    composition = {}
    for item in plan:
        composition[item["kind"]] = composition.get(item["kind"], 0) + 1
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
                 "composition": composition},
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


def _print_failures(records, summary):
    """Violations, errors and check failures, one line each, never hidden."""
    print("  violations      : %d" % summary["violations"])
    print("  errors          : %d (up to %d attempts per cycle, %d retried)"
          % (summary["errors"], summary.get("max_attempts", 1),
             summary.get("cycles_retried", 0)))
    print("  check failures  : %d%s" % (summary.get("checks_failed", 0),
          "" if not summary.get("check_names")
          else " (%s)" % ", ".join(sorted(summary["check_names"]))))
    if summary.get("upstream_findings"):
        print("  upstream finding: %d cycle(s) hit the export-ordering defect "
              "of %s -- present with or without a cut, reported not hidden"
              % (summary["upstream_findings"],
                 ", ".join(summary.get("upstream_checks", []))))
    for record in records:
        if record.get("error"):
            print("    first error   : cycle %s: %s"
                  % (record["cycle"], record["error"]))
            break
    for record in records:
        for item in record.get("violations") or []:
            print("    violation     : cycle %s: %s" % (record["cycle"], item))


def _print_fidelity(summary):
    """How the class A comparison against the injected data went."""
    skew, drift = summary.get("harness_skew_ms", {}), \
        summary.get("harness_drift_ms", {})
    print("  class A fidelity: %d cycle(s) strict (row slop <= %d ms), %d with "
          "a harness lag (max %s ms), %d with a producer-stall drift (max %s ms)"
          % (summary.get("class_a_strict", 0), ROW_SLOP_MS, skew.get("count", 0),
             skew.get("max", "-"), drift.get("count", 0), drift.get("max", "-")))


def _print_stat_line(title, stat, fmt):
    if stat.get("count"):
        print("  %-16s: min %s  median %s  p99 %s  max %s  (n=%d)"
              % (title, fmt % stat["min"], fmt % stat["median"],
                 fmt % stat["p99"], fmt % stat["max"], stat["count"]))


def _print_timings(args, records, summary, elapsed_s):
    _print_stat_line("freeze (ms)", summary["freeze_ms"], "%.3f")
    _print_stat_line("cycle time (s)", summary["cycle_s"], "%.2f")
    if elapsed_s > 0 and summary["cuts"]:
        print("  throughput      : %.1f cuts/min (%.1f s wall, %.2f s/cut "
              "with %d job(s))" % (60.0 * summary["cuts"] / elapsed_s,
                                   elapsed_s, elapsed_s / summary["cuts"],
                                   args.jobs))
    kills = [record["kill_at_ms"] / 1000.0 for record in records
             if isinstance(record.get("kill_at_ms"), (int, float))]
    print(histogram_text(kills,
                         "cut instant histogram (s after the replay started)"),
          end="")
    print(histogram_text([record["cycle_s"] for record in records
                          if isinstance(record.get("cycle_s"), (int, float))],
                         "cycle time histogram"), end="")


def print_summary(args, records, summary, elapsed_s):
    """Readable recap, with the honesty note and the histograms."""
    print("")
    print("process-cut campaign — backend=%s, cut=%s" % (BACKEND, CUT_KIND))
    print("  cycles executed : %d / %d requested (jobs=%d)"
          % (summary["cuts"], args.cycles, args.jobs))
    _print_counts("verdicts", summary["verdicts"])
    _print_counts("cut windows", summary["windows"])
    _print_failures(records, summary)
    _print_fidelity(summary)
    _print_timings(args, records, summary, elapsed_s)
    print("  report          : %s" % args.report)
    print("  state           : %s" % args.state)
    if summary["violations"] or summary.get("checks_failed"):
        print("  artifacts kept  : %s" % args.violation_dir)
    print("")
    print("  %s" % HONESTY)
    print("")


def exit_code(summary) -> int:
    """0 only when nothing was lost, nothing was inconsistent, no export
    check failed and no cycle errored."""
    if summary["violations"] or summary["errors"] or summary.get("checks_failed"):
        return 1
    return 0
