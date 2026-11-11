"""Power-cut campaign orchestrator for the Ambre EDR (spec 7.4).

For every cut instant of a sweep, the campaign:

1. starts a guest machine (real QEMU, or the dry-run fake VM),
2. runs the injected scenario (t=0 is "guest ready"),
3. kills QEMU at the chosen instant (simulated power cut),
4. restarts the machine,
5. asks the guest for its exported events (``edrctl list --json`` then
   ``edrctl export`` by default),
6. compares them with what the scenario injected,
7. classifies the outcome: acquired-and-present, absent-as-expected,
   unexpected loss (violation), unexpected presence (violation).

Outputs: a JSON report, a text histogram, and a violation file holding the
exact cut instants — those become non-regression tests (spec 7.7).

Parallel (``--jobs``) and resumable (``--resume``, JSONL state file).

Scenario file format (``#`` starts a comment)::

    duration_ms 800
    inject  0 evt-a        # event injected at t=0 ms
    acquire 20 evt-a       # expected "acquired" at t=20 ms (default: +20 ms)
    inject  60 evt-b

English only, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import statistics
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qemu_vm  # noqa: E402  (same-directory sibling module)

VERDICT_OK = "ok"
VERDICT_LOSS = "unexpected_loss"
VERDICT_PRESENCE = "unexpected_presence"
VERDICT_ERROR = "error"


# -- scenario model --------------------------------------------------------

class Scenario:
    """Injection plan: which events are written, and when they are acquired."""

    def __init__(self, injections, acquisitions, duration_ms, default_acquire_ms):
        self.injections = injections        # [(t_ms, event_id), ...]
        self.acquisitions = acquisitions    # {event_id: expected t_ms}
        self.duration_ms = duration_ms
        self.default_acquire_ms = default_acquire_ms

    def acquisition_time(self, event_id, injected_at):
        return self.acquisitions.get(event_id, injected_at + self.default_acquire_ms)

    def expected_at(self, cut_ms):
        """Return (expected_present, expected_absent) event ids for a cut."""
        present, absent = [], []
        for t_ms, event_id in self.injections:
            if t_ms > cut_ms:
                continue  # not injected yet: no expectation at all
            target = self.acquisition_time(event_id, t_ms)
            (present if target <= cut_ms else absent).append(event_id)
        return present, absent

    def event_plan(self):
        """Guest-side plan: [{'at_ms': ..., 'event': {...}}, ...]."""
        plan = [{"at_ms": self.acquisition_time(event_id, t_ms),
                 "event": {"id": event_id, "complete": True, "injected_at_ms": t_ms}}
                for t_ms, event_id in self.injections]
        plan.sort(key=lambda item: item["at_ms"])
        return plan


def parse_scenario(path, default_acquire_ms=20):
    """Parse a scenario text file into a :class:`Scenario`."""
    injections, acquisitions, duration_ms = [], {}, 500
    with open(path) as handle:
        for lineno, raw in enumerate(handle, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            key = parts[0].lower()
            try:
                if key in ("duration_ms", "duration-ms") and len(parts) == 2:
                    duration_ms = int(parts[1])
                elif key in ("inject", "e") and len(parts) == 3:
                    injections.append((int(parts[1]), parts[2]))
                elif key in ("acquire", "a") and len(parts) == 3:
                    acquisitions[parts[2]] = int(parts[1])
                else:
                    raise ValueError("unknown directive %r" % line)
            except ValueError as exc:
                raise SystemExit("%s:%d: %s" % (path, lineno, exc))
    if not injections:
        raise SystemExit("%s: scenario has no 'inject' line" % path)
    return Scenario(injections, acquisitions, duration_ms, default_acquire_ms)


# -- classification --------------------------------------------------------

def classify(expected_present, expected_absent, observed_ids, cut_ms):
    """Compare observed event ids with the expectation for a cut instant."""
    observed = set(observed_ids)
    details = {"cut_ms": cut_ms, "present_as_expected": [], "absent_as_expected": [],
               "unexpected_loss": [], "unexpected_presence": []}
    for event_id in expected_present:
        details["present_as_expected" if event_id in observed else "unexpected_loss"] \
            .append(event_id)
    for event_id in expected_absent:
        details["unexpected_presence" if event_id in observed else "absent_as_expected"] \
            .append(event_id)
    details["unknown_events"] = sorted(observed - set(expected_present)
                                       - set(expected_absent))
    if details["unexpected_loss"]:
        return VERDICT_LOSS, details
    if details["unexpected_presence"]:
        return VERDICT_PRESENCE, details
    return VERDICT_OK, details


# -- one cut run -----------------------------------------------------------

def make_vm(args, scenario, cut_ms, workdir):
    """Build the machine used for one run (real QEMU or dry-run stand-in)."""
    return qemu_vm.QemuVM(
        image=args.image, backend=args.backend, workdir=workdir,
        boot_timeout_s=args.boot_timeout, console_prompt=args.prompt,
        mem_mb=args.mem, cpus=args.cpus, blkdebug_config=args.blkdebug_config,
        nbd_socket=args.nbd_socket, cache=args.cache, aio=args.aio,
        dry_run_ready_s=args.dry_run_ready_s,
        dry_run_event_plan=scenario.event_plan(),
        dry_run_cut_ms=cut_ms, dry_run_unflushed_ms=args.dry_run_unflushed_ms)


def collect_guest_events(vm, args):
    """Ask the guest for its events: list first, then export (spec 7.4)."""
    listing = vm.run_guest(args.list_cmd, timeout_s=args.guest_timeout)
    events = qemu_vm.parse_events(listing.stdout)
    if not events:
        return events
    export = vm.run_guest(args.export_cmd, timeout_s=args.guest_timeout)
    return qemu_vm.parse_events(export.stdout) or events


def run_one_cut(args, scenario, cut_ms, cycle):
    """Run a single cut: boot, scenario, cut, restart, collect, classify."""
    workdir = os.path.join(args.workdir, "cut-%d-%d-%d" % (cut_ms, cycle, os.getpid()))
    os.makedirs(workdir, exist_ok=True)
    vm = make_vm(args, scenario, cut_ms, workdir)
    result = {"cut_ms": cut_ms, "cycle": cycle, "verdict": VERDICT_ERROR,
              "boot_s": None, "cycle_s": None, "observed": [], "detail": None,
              "error": None}
    try:
        result["boot_s"] = vm.start()
        # t=0 of the scenario is "guest ready"; a negative instant means the
        # cut lands during boot, so we cut as soon as the guest answers.
        qemu_vm.sleep_slices(max(0.0, cut_ms / 1000.0))
        cut_seconds = vm.power_cut()
        boot_seconds = vm.restart()
        events = collect_guest_events(vm, args)
        observed = sorted({str(ev.get("id")) for ev in events
                           if ev.get("id") is not None})
        verdict, detail = classify(*scenario.expected_at(cut_ms), observed, cut_ms)
        detail["incomplete_events"] = sorted(str(ev.get("id")) for ev in events
                                             if ev.get("complete") is False)
        result.update(verdict=verdict, observed=observed, detail=detail,
                      cycle_s=cut_seconds + boot_seconds)
    except (qemu_vm.PowercutError, TimeoutError, OSError) as exc:
        result["error"] = "%s: %s" % (type(exc).__name__, exc)
    finally:
        vm.stop()
    return result


# -- sweep instants + resumable state --------------------------------------

def build_cut_list(args):
    """Deterministic sweep instants plus optional random cuts."""
    cuts = list(range(args.sweep_from_ms, args.sweep_to_ms + 1, args.step_ms))
    if args.random_cuts > 0:
        rng = random.Random(args.seed)
        span = max(1, args.sweep_to_ms - args.sweep_from_ms)
        cuts += [rng.randint(args.sweep_from_ms, args.sweep_from_ms + span)
                 for _ in range(args.random_cuts)]
    return cuts


class StateStore:
    """Append-only JSONL state file: what has already been done (--resume)."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.done = {}
        if os.path.exists(path):
            with open(path) as handle:
                for line in handle:
                    try:
                        record = json.loads(line)
                    except ValueError:
                        continue
                    self.done[(record.get("cut_ms"), record.get("cycle"))] = record

    def has(self, cut_ms, cycle):
        return (cut_ms, cycle) in self.done

    def append(self, record):
        with self.lock:
            self.done[(record["cut_ms"], record["cycle"])] = record
            with open(self.path, "a") as handle:
                handle.write(json.dumps(record, sort_keys=True) + "\n")

    def records(self):
        return sorted(self.done.values(),
                      key=lambda r: (r.get("cut_ms", 0), r.get("cycle", 0)))


# -- reporting -------------------------------------------------------------

def histogram_text(values, title, buckets=8):
    """Render a plain-text histogram (no third-party plotting library)."""
    if not values:
        return "%s: no samples\n" % title
    low, high = min(values), max(values)
    if high <= low:
        return "%s: all %d samples = %.3fs\n" % (title, len(values), low)
    width = (high - low) / buckets
    counts = [0] * buckets
    for value in values:
        counts[min(buckets - 1, int((value - low) / width))] += 1
    peak = max(counts) or 1
    lines = ["%s (%d samples)" % (title, len(values))]
    for index, count in enumerate(counts):
        start = low + index * width
        bar = "#" * int(round(40.0 * count / peak))
        lines.append("  %7.3f-%7.3f s | %-40s %d" % (start, start + width, bar, count))
    return "\n".join(lines) + "\n"


def stats(values):
    """min / median / max of a sample list (None when empty)."""
    if not values:
        return None
    ordered = sorted(values)
    return {"count": len(ordered), "min": round(ordered[0], 4),
            "median": round(statistics.median(ordered), 4),
            "max": round(ordered[-1], 4)}


def summarise(records):
    """Aggregate verdicts and timings over every recorded run."""
    verdicts = {}
    for record in records:
        verdict = record.get("verdict", VERDICT_ERROR)
        verdicts[verdict] = verdicts.get(verdict, 0) + 1
    cycles = [r["cycle_s"] for r in records
              if isinstance(r.get("cycle_s"), (int, float))]
    return {"cuts": len(records), "verdicts": verdicts,
            "violations": verdicts.get(VERDICT_LOSS, 0)
            + verdicts.get(VERDICT_PRESENCE, 0),
            "errors": verdicts.get(VERDICT_ERROR, 0), "cycle_s": stats(cycles)}


def write_reports(args, records):
    """Write the JSON report and the violation file (spec 7.7)."""
    summary = summarise(records)
    report = {
        "tool": "powercut_campaign", "backend": args.backend, "image": args.image,
        "scenario": args.scenario,
        "sweep": {"from_ms": args.sweep_from_ms, "to_ms": args.sweep_to_ms,
                  "step_ms": args.step_ms, "random_cuts": args.random_cuts,
                  "cycles": args.cycles, "seed": args.seed},
        "guest_commands": {"list": args.list_cmd, "export": args.export_cmd},
        "summary": summary, "runs": records,
    }
    with open(args.report, "w") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    with open(args.violation_file, "w") as handle:
        for record in records:
            if record.get("verdict") in (VERDICT_LOSS, VERDICT_PRESENCE):
                handle.write(json.dumps({
                    "cut_ms": record["cut_ms"], "cycle": record["cycle"],
                    "verdict": record["verdict"], "detail": record.get("detail"),
                    "observed": record.get("observed"),
                    "scenario": os.path.abspath(args.scenario),
                }, sort_keys=True) + "\n")
    return summary


def print_summary(summary, records, args):
    cycles = [r["cycle_s"] for r in records
              if isinstance(r.get("cycle_s"), (int, float))]
    print("power-cut campaign — backend=%s" % args.backend)
    print("  cuts executed      : %d" % summary["cuts"])
    print("  verdicts           : %s" % json.dumps(summary["verdicts"], sort_keys=True))
    print("  violations         : %d" % summary["violations"])
    print("  errors             : %d" % summary["errors"])
    if summary["cycle_s"]:
        print("  cycle time (s)     : min %.3f  median %.3f  max %.3f"
              % (summary["cycle_s"]["min"], summary["cycle_s"]["median"],
                 summary["cycle_s"]["max"]))
    print(histogram_text(cycles, "cycle time histogram"), end="")
    print("  report             : %s" % args.report)
    print("  violations file    : %s" % args.violation_file)


# -- campaign driver -------------------------------------------------------

def execute(args, scenario, state, cuts):
    """Run (or resume) every cut, honouring --jobs and --cycles."""
    tasks = [(cut_ms, cycle) for cut_ms in cuts for cycle in range(args.cycles)]
    pending = [task for task in tasks if not state.has(*task)]
    if len(pending) != len(tasks):
        print("resume: %d task(s) already done, %d to run"
              % (len(tasks) - len(pending), len(pending)))
    if not pending:
        return
    step = max(1, len(pending) // 10)
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(run_one_cut, args, scenario, cut_ms, cycle)
                   for cut_ms, cycle in pending]
        for index, future in enumerate(futures, 1):
            state.append(future.result())
            if index % step == 0 or index == len(pending):
                print("  %d/%d done" % (index, len(pending)))


def build_parser():
    parser = argparse.ArgumentParser(description="Ambre power-cut campaign (spec 7.4)")
    parser.add_argument("--image", default=None, help="guest disk image (.img)")
    parser.add_argument("--scenario", required=True, help="scenario text file")
    parser.add_argument("--sweep-from-ms", type=int, default=-50)
    parser.add_argument("--sweep-to-ms", type=int, default=500)
    parser.add_argument("--step-ms", type=int, default=1)
    parser.add_argument("--random-cuts", type=int, default=0)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--backend", choices=("qemu", "dry-run"), default="qemu")
    parser.add_argument("--report", default="powercut-report.json")
    parser.add_argument("--violation-file", default=None)
    parser.add_argument("--state", default=None, help="JSONL state file (--resume)")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--list-cmd", default=qemu_vm.DEFAULT_LIST_CMD)
    parser.add_argument("--export-cmd", default=qemu_vm.DEFAULT_EXPORT_CMD)
    parser.add_argument("--guest-timeout", type=float, default=60.0)
    parser.add_argument("--boot-timeout", type=float, default=qemu_vm.DEFAULT_BOOT_TIMEOUT_S)
    parser.add_argument("--prompt", default=qemu_vm.DEFAULT_PROMPT)
    parser.add_argument("--mem", type=int, default=512)
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--cache", default="none")
    parser.add_argument("--aio", default="native")
    parser.add_argument("--blkdebug-config", default=None)
    parser.add_argument("--nbd-socket", default=None)
    parser.add_argument("--default-acquire-ms", type=int, default=20)
    parser.add_argument("--dry-run-ready-s", type=float, default=0.03)
    parser.add_argument("--dry-run-unflushed-ms", type=float, default=0.0,
                        help="dry-run: window of writes lost at the cut (0 = none)")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.violation_file = args.violation_file or args.report + ".violations.jsonl"
    args.state = args.state or args.report + ".state.jsonl"
    args.workdir = args.workdir or tempfile.mkdtemp(prefix="ambre-campaign-")
    if args.backend == "qemu" and not args.image:
        raise SystemExit("--image is required with --backend qemu")
    os.makedirs(args.workdir, exist_ok=True)
    if args.resume and not os.path.exists(args.state):
        print("resume: no state file at %s, starting from scratch" % args.state)
    scenario = parse_scenario(args.scenario, args.default_acquire_ms)
    state = StateStore(args.state)
    cuts = build_cut_list(args)
    print("sweep: %d instant(s) from %d ms to %d ms (step %d ms), cycles=%d, jobs=%d"
          % (len(cuts), args.sweep_from_ms, args.sweep_to_ms, args.step_ms,
             args.cycles, args.jobs))
    execute(args, scenario, state, cuts)
    records = state.records()
    summary = write_reports(args, records)
    print_summary(summary, records, args)
    return 0 if summary["violations"] == 0 and summary["errors"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
