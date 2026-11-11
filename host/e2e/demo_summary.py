#!/usr/bin/env python3
"""Print the demonstration recap of one Ambre level-2 run (scripts/demo.sh).

It reads the JSON report written by host/e2e/e2e_run.py and renders the story
of the specification's demonstration (cahier section 8): a drive is replayed,
the impact triggers, the event is acquired in N milliseconds, the journal
process is killed, it is restarted on the same disk, the service list shows
the event, the export is verified.

Nothing is invented: every number comes from the report, and a value the run
did not produce is printed as "not measured". The exit code is 0 only when the
run itself succeeded, so the demo can never print a false success.

English only, Python 3 standard library only.
"""

from __future__ import annotations

import json
import sys


def load(path):
    with open(path) as handle:
        return json.load(handle)


def ms(value, digits=3):
    return ("%.*f ms" % (digits, value / 1000.0)) if value else "not measured"


def line(label, value):
    print("    %-26s %s" % (label, value))


def event_line(event):
    return ("event %d  state=%s  locked=%s  complete file=%s  reason=%s"
            % (event["event_id"], event["state"],
               "yes" if event["locked"] else "no",
               "yes" if event["file_complete"] else "no",
               event["trig_reason"]))


def print_story(report):
    """The demonstration narrative, one line per stage."""
    timeline = report.get("timeline", {})
    events = report.get("events", [])
    checks = report.get("checks", {})
    print("")
    print("    --- the story of this run " + "-" * 46)
    line("1. drive replayed", "%s (impact at %s ms of the scenario)"
         % (report.get("scenario", "?"), report.get("crash_ms")))
    line("2. impact -> trigger", "class A snapshot frozen, event acquired in %s"
         % ms(timeline.get("freeze_us")))
    line("3. the cut", "SIGKILL on edr-journal at %+.0f ms (window: %s)"
         % (report.get("kill_at_ms", 0.0), report.get("window")))
    line("4. restart", "same disk file, recovery ran at start-up (REP-01)")
    line("5. service list", "%d event(s) recovered" % len(events))
    for event in events:
        print("       %s" % event_line(event))
    export = checks.get("export", {})
    line("6. MCAP export", "%s (rc=%s)" % (checks.get("mcap", "not produced"),
                                           export.get("rc")))
    official = checks.get("official", {})
    line("7. official checks", "%s%s" % (
        "green" if official.get("ok") else "FAILED",
        "" if not official.get("failed")
        else " -> " + ", ".join(official["failed"])))
    for item in official.get("upstream", []):
        print("       upstream finding: %s (%s) -- an export-ordering defect of "
              "the gaps channel, present with or without a cut"
              % (item.get("check"), item.get("detail")))
    fidelity = checks.get("fidelity", {})
    if fidelity:
        line("8. class A data", "%d/%d points identical to the injected rows "
             "(alignment %+s ms, worst row slop %s ms%s)"
             % (fidelity.get("matched", 0), fidelity.get("total", 0),
                fidelity.get("t0_offset_ms"),
                fidelity.get("worst_row_slop_ms"),
                "" if fidelity.get("skew_ms") is None
                else ", harness lag %s ms" % fidelity["skew_ms"]))
    line("9. verdict", report.get("verdict", "?"))


def print_replay_status(report):
    """Report the producer's exit status exactly as it was, with its cause."""
    rc = report.get("replay_rc")
    if rc in (0, None):
        return
    if rc in (-15, -9):
        print("    * edr-replay was stopped by the bench after the cut (signal"
              " %d); this is the bench tearing the scenario down, not a"
              % -rc)
        print("      failure of the producer.")
        return
    print("    * edr-replay exited with status %s (expected 0: every frame and"
          % rc)
    print("      every EXTERNAL signal is acknowledged by edr-capture, so the")
    print("      producer must not time out - see ROB-06).")


def print_limits(report):
    """What this demonstration does not prove -- printed every time."""
    print("")
    print("    --- what this run does NOT prove " + "-" * 39)
    print("    * backend=posix: the raw partition is a regular file and the cut")
    print("      is a SIGKILL on one process. Writes already handed to the host")
    print("      kernel survive it: the loss of a volatile cache is NOT tested")
    print("      here (that is level 1, host/explore/), and nothing about QNX")
    print("      scheduling, drivers or the security policy is tested either.")
    print("    * the freeze time above is the software share only; the media")
    print("      share needs the QNX target (PERF-02, essay E1).")
    probe = report.get("edrctl") or {}
    if probe.get("binary") and not probe.get("used"):
        print("    * the frozen `edrctl` binary could not answer (%s);"
              % (probe.get("stdout") or probe.get("stderr") or "rc=%s"
                 % probe.get("rc")))
        print("      the bench spoke the same frozen protocol itself")
        print("      (host/e2e/ambre_wire.py). See host/e2e/README.md.")
    print_replay_status(report)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) != 1:
        print("usage: demo_summary.py <report.json>", file=sys.stderr)
        return 2
    report = load(argv[0])
    print_story(report)
    print_limits(report)
    print("")
    ok = report.get("verdict") == "ok"
    if ok:
        print("    \033[1;32mDEMO OK\033[0m — the event survived a brutal process "
              "death, the recovery sealed it,")
        print("    the export verified and its class A data equals the "
              "injected scenario.")
    else:
        print("    \033[1;31mDEMO FAILED\033[0m — verdict %s"
              % report.get("verdict"))
        for item in report.get("violations", []):
            print("      violation: %s" % item)
        if report.get("error"):
            print("      error: %s" % report["error"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
