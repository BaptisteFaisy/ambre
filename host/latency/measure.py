#!/usr/bin/env python3
"""measure.py — the Ambre latency campaign harness (backend=posix).

Drives successive triggers through the real PC chain (edr-journal +
edr-capture + edr-replay), under an optional load, and measures:

  PERF-01  crossing sample -> trigger pulse        (limit 2 ms)
  PERF-02  trigger -> "acquis", total freeze        (limit 20 ms, target HW)
  PERF-03  software share of that freeze            (limit 500 us)
  PERF-05  cost of sending one frame, producer side (limit 50 us at p99)

and decomposes the freeze into software (measured with the journal file on
tmpfs) and media (derived: total - software).  Every number is a measurement
of this host with this backend; nothing is extrapolated.

Usage:
    python3 host/latency/measure.py --bins build --scenario sc.txt \
        --triggers 200 [--load cpu|disk|mem|none] [--report out.json] [--quick]

The report layout and the human output live in host/latency/report.py.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "host", "mcap"))

import campaign                                                   # noqa: E402
import chain as chain_mod                                         # noqa: E402
import load_gen                                                   # noqa: E402
import report as report_mod                                       # noqa: E402

QUIET_ROWS_FULL = 10000        # 10 s of quiet driving, synthetic fallback
QUIET_ROWS_QUICK = 1000        # 1 s in --quick mode
QUIET_SCENARIO = os.path.join(ROOT, "host", "scenarios", "no-crash.txt")
QUIET_ATTEMPTS = 3
QUIET_TIMEOUT_S = 240.0
QUICK_TRIGGERS = 4


# ------------------------------------------------------------------ layout --

def tmpfs_root():
    """/dev/shm when usable (the software pass needs a flush-free support)."""
    candidate = "/dev/shm"
    if os.path.isdir(candidate) and os.access(candidate, os.W_OK):
        return candidate
    return None


def require_binaries(bins):
    missing = [name for name in ("edr-journal", "edr-capture", "edr-replay")
               if not os.access(os.path.join(bins, name), os.X_OK)]
    if missing:
        raise SystemExit(f"missing binaries in {bins}: {', '.join(missing)} "
                         "(build them with: make BUILD=<dir> processes)")


def estimate_seconds(triggers, quiet_rows):
    """Rough budget of the load window: two passes of chunks + the quiet run."""
    chunks = max(1, -(-triggers // campaign.EVENTS_PER_CHUNK))
    return int(2 * chunks * 4 + quiet_rows / 1000.0 + 90)


def count_journal_records(disk_path):
    """Record types written in a journal image: {rec_type: count}.

    Used by the PERF-05 reference run: a quiet drive writes no event, so any
    record there is a GAP the capture forwarded synchronously to the journal,
    which is what blocks the producer's send on those frames.
    """
    counts = {}
    try:
        from ambre_journal_reader import JournalReader
        for record in JournalReader(disk_path).records():
            kind = record.get("rec_type")
            counts[kind] = counts.get(kind, 0) + 1
    except Exception:                                       # noqa: BLE001
        return counts
    return counts


# ------------------------------------------------------------------- runs --

def run_quiet(bins, workdir, log_dir, quiet_path):
    """PERF-05 reference run: a shock-free drive, no trigger on the path.

    Retried once: a single 500 ms stall of the input path (edr-replay's fixed
    send timeout) aborts a run, and that is a property of the transport, not a
    measurement.
    """
    disk = os.path.join(workdir, "journal-perf05.img")
    parsed = {"messages": 0, "rc": None, "scenario": quiet_path}
    for attempt in range(QUIET_ATTEMPTS):
        tag = f"perf05-{attempt}"
        chain = chain_mod.start_chain(bins, workdir, tag, disk, log_dir)
        try:
            rc, output = chain_mod.run_replay(bins, chain, quiet_path,
                                              timeout=QUIET_TIMEOUT_S)
        finally:
            chain_mod.stop_chain(chain)
        with open(os.path.join(log_dir, f"replay-{tag}.log"), "w",
                  encoding="utf-8") as handle:
            handle.write(output)
        parsed = chain_mod.parse_replay_output(output)
        parsed["rc"] = rc
        parsed["attempt"] = attempt + 1
        parsed["scenario"] = quiet_path
        parsed["journal_records"] = count_journal_records(disk)
        if rc == 0:
            break
    return parsed


def run_passes(bins, workdir, log_dir, chunks, triggers):
    """The media pass (NVMe file) and the software pass (tmpfs), plus the
    shock-free PERF-05 run.  The load is already running when called."""
    media = campaign.run_pass(bins, workdir,
                              os.path.join(workdir, "journal-media.img"),
                              chunks, triggers, log_dir, tag="media")
    tmpfs = tmpfs_root()
    software = None
    software_disk = None
    if tmpfs:
        software_disk = os.path.join(tmpfs, f"ambre-latency-{os.getpid()}.img")
        try:
            software = campaign.run_pass(bins, workdir, software_disk, chunks,
                                         triggers, log_dir, tag="software")
        finally:
            if os.path.exists(software_disk):
                os.unlink(software_disk)
    return media, software, software_disk


# --------------------------------------------------------------------- CLI --

def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bins", default="build",
                        help="build directory holding edr-journal/capture/replay")
    parser.add_argument("--scenario", required=True,
                        help="multi-shock scenario (host/latency/multi_shock.py)")
    parser.add_argument("--triggers", type=int, required=True,
                        help="number of trigger events to measure")
    parser.add_argument("--load", default="none",
                        choices=["cpu", "disk", "mem", "none"])
    parser.add_argument("--report", default=None, help="JSON report path")
    parser.add_argument("--quick", action="store_true",
                        help="short campaign for the test suite")
    parser.add_argument("--perf05-scenario", default=None,
                        help="shock-free scenario for the PERF-05 reference "
                             "run (default: host/scenarios/no-crash.txt)")
    parser.add_argument("--workdir", default=None,
                        help="keep the working directory here (default: a "
                             "temporary one, removed at the end)")
    parser.add_argument("--keep-workdir", action="store_true")
    return parser.parse_args(argv)


def prepare(args):
    """Binaries, triggers, workdir, chunks and the PERF-05 scenario."""
    bins = os.path.abspath(args.bins)
    require_binaries(bins)
    if args.triggers < 1:
        raise SystemExit("--triggers must be >= 1")
    triggers = min(args.triggers, QUICK_TRIGGERS) if args.quick else args.triggers
    workdir = args.workdir or tempfile.mkdtemp(prefix="ambre-latency-")
    log_dir = os.path.join(workdir, "logs")
    os.makedirs(log_dir, exist_ok=True)
    chunks, shocks = campaign.build_chunks(args.scenario, workdir)
    if not chunks:
        raise SystemExit(f"no shock found in {args.scenario}: the campaign "
                         "needs at least one (host/latency/multi_shock.py)")
    quiet_path = args.perf05_scenario
    if quiet_path is None and not args.quick and os.path.exists(QUIET_SCENARIO):
        quiet_path = QUIET_SCENARIO
    if quiet_path is None:
        quiet_path = os.path.join(workdir, "quiet.txt")
        rows = QUIET_ROWS_QUICK if args.quick else QUIET_ROWS_FULL
        campaign.write_quiet_scenario(quiet_path, rows)
    return bins, triggers, workdir, log_dir, chunks, shocks, quiet_path


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    bins, triggers, workdir, log_dir, chunks, shocks, quiet_path = prepare(args)
    own_workdir = args.workdir is None
    procs, load_desc = load_gen.start_load(
        args.load, estimate_seconds(triggers, len(chunks) * 220),
        directory=os.path.join(workdir, "archive"))
    try:
        media, software, software_disk = run_passes(bins, workdir, log_dir,
                                                    chunks, triggers)
        quiet = run_quiet(bins, workdir, log_dir, quiet_path)
    finally:
        load_gen.stop_load(procs)
    report = report_mod.build_report(args, bins, workdir, load_desc, chunks,
                                     shocks, media, software, quiet,
                                     software_disk)
    report_mod.print_human(report)
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)) or ".",
                    exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
        print(f"\nreport written to {args.report}")
    if own_workdir and not args.keep_workdir:
        shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
