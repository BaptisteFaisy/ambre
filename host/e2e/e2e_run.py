#!/usr/bin/env python3
"""Ambre level-2 bench: one end-to-end run with one real SIGKILL.

    python3 host/e2e/e2e_run.py --bins build --scenario scen.txt \
            --cut-at acquired [--out report.json]

What it does, in order: start edr-journal on a fresh disk file, start
edr-capture, replay the scenario through edr-replay, wait until the event
is acquired (and, depending on the cut, until it is sealed), SIGKILL the
journal, restart it on the SAME disk file (the recovery must run), read the
recovered state, export the event to MCAP and check it with the official
verifier.

What it proves: the whole software chain of the POSIX backend survives a
brutal process death -- the acquisition, the recovery, the export and the
verification. What it does NOT prove: anything about a QNX target, and
anything about the loss of a volatile cache (see host/e2e/README.md).

English only, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import e2e_cycle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

BACKEND_NOTE = (
    "backend=posix: the journal partition is a regular file, the cut is a "
    "SIGKILL on the edr-journal process (real processes, real files, real "
    "signals). This is NOT a QEMU/QNX power cut: writes already handed to "
    "the host kernel survive the kill, so this bench does not exercise the "
    "loss of a volatile cache (that is the level-1 exhaustive exploration, "
    "host/explore/).")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Ambre level-2 end-to-end bench (real processes, real SIGKILL)")
    parser.add_argument("--bins", default="build",
                        help="directory holding edr-journal/edr-capture/edr-replay")
    parser.add_argument("--scenario", default=None,
                        help="scenario file (docs/FORMAT.md section 11)")
    parser.add_argument("--cut-at", default="acquired",
                        help="pre | acquired | sealed | recovery | random | <ms>")
    parser.add_argument("--out", default=None, help="write the JSON report here")
    parser.add_argument("--rate", type=float, default=1.0,
                        help="replay rate (1.0 = real time, the default)")
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--duration-ms", type=int, default=6000)
    parser.add_argument("--crash-at-ms", type=int, default=3000)
    parser.add_argument("--keep", action="store_true",
                        help="keep the working directory (disk, sockets, logs)")
    parser.add_argument("--workdir", default=None,
                        help="run in this directory instead of a temporary one")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip the MCAP export and the verification")
    return parser.parse_args(argv)


def ensure_scenario(args) -> str:
    """Return the scenario to play: the given one, or a generated one."""
    if args.scenario:
        if not os.path.isfile(args.scenario):
            raise SystemExit("error: scenario not found: %s" % args.scenario)
        return args.scenario
    path = os.path.join(tempfile.gettempdir(),
                        "ambre-e2e-scenario-%d.txt" % args.seed)
    cmd = [sys.executable, os.path.join("host", "scenarios", "generate.py"),
           "--seed", str(args.seed), "--duration-ms", str(args.duration_ms),
           "--crash-at-ms", str(args.crash_at_ms), "--out", path]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit("error: cannot generate the scenario:\n%s"
                         % proc.stderr.strip())
    return path


def print_events(result: dict) -> None:
    """The recovered events, one line each, plus the primary one."""
    events = result.get("events", [])
    primary = e2e_cycle.primary_event(events)
    print("events recovered : %d" % len(events))
    for event in events:
        print("  event %-4d %-6s locked=%-3s complete_file=%-3s reason=%-13s"
              % (event["event_id"], event["state"],
                 "yes" if event["locked"] else "no",
                 "yes" if event["file_complete"] else "no",
                 event["trig_reason"]))
    if primary:
        print("primary event    : id=%d state=%s locked=%s complete_file=%s"
              % (primary["event_id"], primary["state"],
                 "yes" if primary["locked"] else "no",
                 "yes" if primary["file_complete"] else "no"))


def print_probe(result: dict) -> None:
    """Which service probe answered: the real edrctl, or the wire client."""
    probe = result.get("edrctl") or {}
    if not probe.get("binary"):
        return
    if probe.get("used"):
        print("service probe    : %s answered (rc=0)" % probe["binary"])
        return
    first = (probe.get("stdout") or probe.get("stderr")
             or "(no output)").splitlines()[0][:110]
    print("service probe    : %s answered rc=%s: %s"
          % (probe["binary"], probe.get("rc"), first))
    print("                   -> the bench used its own client of the same "
          "frozen protocol (host/e2e/ambre_wire.py)")


def print_checks(checks: dict) -> None:
    """Export, official verifier and class A comparison, as they came."""
    official = checks.get("official", {})
    fidelity = checks.get("fidelity", {})
    print("mcap export      : rc=%s" % checks.get("export", {}).get("rc"))
    print("official checks  : %s%s" % (
        "green" if official.get("ok") else "FAILED",
        "" if not official.get("failed")
        else " (%s)" % ", ".join(official.get("failed", []))))
    for item in official.get("upstream", []):
        print("  upstream finding: %s -- %s (export-ordering defect of %s, "
              "unrelated to the cut)" % (item.get("check"), item.get("detail"),
                                         ", ".join(item.get("topics", {}))))
    lag = fidelity.get("skew_ms")
    drift = fidelity.get("worst_drift_ms")
    extra = ""
    if lag:
        extra += ", harness lag %s ms" % lag
    if drift:
        extra += ", producer stall drift %s ms" % drift
    print("class A fidelity : %s (%d/%d points, %d exact, alignment %s ms, "
          "worst row slop %s ms%s)"
          % ("green" if fidelity.get("ok") else "FAILED",
             fidelity.get("matched", 0), fidelity.get("total", 0),
             fidelity.get("matched_exact_ms", 0), fidelity.get("t0_offset_ms"),
             fidelity.get("worst_row_slop_ms"), extra))
    for problem in (fidelity.get("problems") or [])[:2]:
        print("                   %s" % problem)


def print_summary(result: dict) -> None:
    """Readable recap of one run."""
    freeze = result.get("timeline", {}).get("freeze_us")
    print("")
    print("cut point        : %s (%s, at %+.0f ms after the replay started)"
          % (result["cut"], result["window"], result.get("kill_at_ms", 0.0)))
    if freeze is not None:
        print("acquired in      : %.3f ms (trigger -> flush, software share)"
              % (freeze / 1000.0))
    print_events(result)
    print_probe(result)
    if result.get("checks"):
        print_checks(result["checks"])
    print("verdict          : %s" % result["verdict"])
    for item in result.get("violations", []):
        print("  violation      : %s" % item)
    print("")


def main(argv=None) -> int:
    args = parse_args(argv)
    scenario = ensure_scenario(args)
    if args.workdir:
        os.makedirs(args.workdir, exist_ok=True)
    result = e2e_cycle.run_cycle(
        args.bins, scenario, args.cut_at, workdir=args.workdir,
        keep=args.keep or bool(args.workdir), rate=args.rate,
        verify=not args.no_verify, seed=args.seed)
    result["backend"] = BACKEND_NOTE
    result["scenario"] = os.path.abspath(scenario)
    result["workdir"] = os.path.abspath(args.workdir or tempfile.gettempdir())
    if args.out:
        with open(args.out, "w") as handle:
            json.dump(result, handle, indent=2, sort_keys=True)
        print("report           : %s" % args.out)
    print_summary(result)
    return 0 if result["verdict"] == e2e_cycle.VERDICT_OK else 1


if __name__ == "__main__":
    sys.exit(main())
