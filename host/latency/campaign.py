#!/usr/bin/env python3
"""Campaign pieces of the latency measurement: chunks, runs, statistics.

Used by host/latency/measure.py and by tests/test_latency_py.py.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "host", "mcap"))

import chain as chain_mod                                        # noqa: E402
import latency_stats as stats                                   # noqa: E402
import load_gen                                                  # noqa: E402

EVENTS_PER_CHUNK = 8            # AMBRE_MAX_EVENTS slots in the alloc table
CHUNK_ATTEMPTS = 3              # retries when edr-replay fails (timeout)


# ------------------------------------------------------------------- hosts --

def cpu_model():
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def mem_total_bytes():
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        pass
    return 0


def compiler_version():
    for candidate in ("gcc", "cc", "clang"):
        try:
            out = subprocess.run([candidate, "--version"], capture_output=True,
                                 text=True, timeout=10)
            if out.returncode == 0 and out.stdout:
                return out.stdout.splitlines()[0].strip()
        except (OSError, subprocess.SubprocessError):
            continue
    return "unknown"


def filesystem_of(path):
    path = os.path.abspath(path)
    while not os.path.exists(path) and path != os.path.dirname(path):
        path = os.path.dirname(path)
    try:
        out = subprocess.run(["stat", "-f", "-c", "%T", path],
                             capture_output=True, text=True, timeout=10)
        if out.returncode == 0:
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown"


def host_info(workdir):
    """The machine the numbers were measured on (no extrapolation)."""
    return {
        "cpu": cpu_model(),
        "cores": os.cpu_count(),
        "ram_total_bytes": mem_total_bytes(),
        "ram_available_bytes": load_gen.mem_available_bytes(),
        "python": platform.python_version(),
        "filesystem": filesystem_of(workdir),
    }


# ----------------------------------------------------------------- chunks --

def build_chunks(scenario_path, workdir):
    """Split the scenario into one chunk per shock and write them out."""
    _header, rows = stats.parse_scenario(scenario_path)
    shocks = stats.find_shocks(rows)
    chunks = []
    for index, shock in enumerate(shocks):
        chunk = stats.build_chunk(rows, shock)
        path = os.path.join(workdir, f"chunk-{index:03d}.txt")
        stats.write_scenario(path, chunk)
        chunks.append(path)
    return chunks, len(shocks)


def write_quiet_scenario(path, rows):
    """A shock-free scenario: the PERF-05 reference (a producer sending
    frames without any trigger blocking the path)."""
    chunk = [stats.quiet_row(index * 1000, 200) for index in range(rows)]
    stats.write_scenario(path, chunk, name="latency-quiet")


def event_metrics(trace, descriptors, run):
    """One record per completed trigger event (a refused trigger has no
    FLUSH_ACK and is therefore not a measurement).

    `is_first_of_crash` marks the event that the shock itself produced; the
    others are re-fires of the same crash (the live capture runs the trigger
    engine in WINDOW mode, which keeps firing while the 150 ms window still
    holds the crash delta-v).  PERF-01 is only comparable for the first one:
    during a re-fire burst the capture blocks on the journal's answer, so its
    interpolated sample timeline drifts behind the wall clock.
    """
    rows = []
    for event_id in sorted(trace):
        points = trace[event_id]
        if 1 not in points or 3 not in points:
            continue
        desc = descriptors.get(event_id, {})
        trig_ts = desc.get("trig_ts_us")
        row = {
            "run": run,
            "event_id": event_id,
            "is_first_of_crash": event_id == 1,
            "trig_ts_us": trig_ts,
            "t_trigger_us": points[1],
            "t_write_submit_us": points.get(2),
            "t_flush_ack_us": points[3],
            "t_seal_us": points.get(4),
            "perf01_us": (points[1] - trig_ts) if trig_ts else None,
            "freeze_us": points[3] - points[1],
            "dispatch_us": (points[2] - points[1]) if 2 in points else None,
            "write_flush_us": (points[3] - points[2]) if 2 in points else None,
            "seal_after_us": (points[4] - points[3]) if 4 in points else None,
            "locked": desc.get("locked"),
            "state": desc.get("state"),
        }
        rows.append(row)
    return rows


def run_chunk(bins, workdir, index, disk_path, chunk_path, log_dir, tag="run"):
    """One journal boot + one replay; returns the measured events."""
    chain = chain_mod.start_chain(bins, workdir, tag, disk_path, log_dir)
    try:
        rc, output = chain_mod.run_replay(bins, chain, chunk_path)
        with open(os.path.join(log_dir, f"replay-{tag}.log"), "w",
                  encoding="utf-8") as handle:
            handle.write(output)
    finally:
        chain_mod.stop_chain(chain)
    descriptors = chain_mod.read_event_descriptors(disk_path)
    trace = chain_mod.parse_trace(chain["trace"])
    status = chain_mod.read_journal_status(
        os.path.join(log_dir, f"journal-{tag}.log"))
    refused = len([p for p in trace.values() if 1 in p and 3 not in p])
    return {"rc": rc, "events": event_metrics(trace, descriptors, index),
            "refused_triggers": refused,
            "histogram": chain_mod.parse_replay_output(output),
            "status": status}


def run_pass(bins, workdir, disk_path, chunks, target, log_dir, tag="media"):
    """Replay the chunks (round robin) until `target` events are measured.

    A chunk that produced no event at all is a miss, not a measurement: it is
    retried like a failed run and recorded in `failures`, so the report never
    hides one.
    """
    events = []
    histograms = []
    refused = 0
    failures = []
    runs = 0
    index = 0
    worst_freeze_us = 0
    while len(events) < target and runs < 2 * target + 8:
        chunk_path = chunks[index % len(chunks)]
        result = {"rc": -1, "events": [], "refused_triggers": 0, "status": {},
                  "histogram": {"messages": 0, "buckets": []}}
        for _attempt in range(CHUNK_ATTEMPTS):
            runs += 1
            result = run_chunk(bins, workdir, runs, disk_path, chunk_path,
                               log_dir, tag=f"{tag}-{runs}")
            if result["rc"] == 0 and result["events"]:
                break
            failures.append({"run": runs, "rc": result["rc"],
                             "measured": len(result["events"])})
        events.extend(result["events"])
        refused += result["refused_triggers"]
        histograms.append(result["histogram"])
        worst_freeze_us = max(worst_freeze_us,
                              int(result["status"].get("worst_freeze_us", 0)))
        index += 1
    return {"events": events, "histograms": histograms,
            "refused_triggers": refused, "runs": runs, "failures": failures,
            "journal_worst_freeze_us": worst_freeze_us}


# -------------------------------------------------------------- histograms --

def pool_histograms(histograms):
    """Pool edr-replay's bucket counts into one bucket set."""
    buckets = {}
    messages = 0
    for histogram in histograms:
        messages += histogram.get("messages", 0)
        for ceiling, count in histogram.get("buckets", []):
            buckets[ceiling] = buckets.get(ceiling, 0) + count
    return {"messages": messages, "buckets": buckets}


def percentile_from_buckets(buckets, messages, pct):
    """Percentile of a bucketed histogram (ceilings, as edr-replay reports)."""
    if messages <= 0 or not buckets:
        return None
    rank = max(1, (messages * int(round(pct * 10)) + 999) // 1000)
    seen = 0
    for ceiling in sorted(buckets):
        seen += buckets[ceiling]
        if seen >= rank:
            return ceiling
    return max(buckets)


def _perf01(group):
    return stats.summarize([e["perf01_us"] for e in group
                            if e.get("perf01_us") is not None])


def _values(group, key):
    return stats.summarize([e[key] for e in group if e.get(key) is not None])


def summarize_pass(result):
    """Statistics of one pass: PERF-01 (crash triggers and re-fires apart),
    the total freeze and its decomposition."""
    events = result["events"]
    first = [e for e in events if e.get("is_first_of_crash")]
    refire = [e for e in events if not e.get("is_first_of_crash")]
    pooled = pool_histograms(result["histograms"])
    return {
        "events_measured": len(events),
        "crash_triggers": len(first),
        "refire_events": len(refire),
        "refused_triggers": result["refused_triggers"],
        "runs": result["runs"],
        "failures": result["failures"],
        "perf01_us": _perf01(first),
        "perf01_refire_us": _perf01(refire),
        "perf01_all_us": _perf01(events),
        "freeze_us": _values(events, "freeze_us"),
        "freeze_first_us": _values(first, "freeze_us"),
        "dispatch_us": _values(events, "dispatch_us"),
        "write_flush_us": _values(events, "write_flush_us"),
        "seal_after_us": _values(events, "seal_after_us"),
        "perf05_send_ns": {
            "messages": pooled["messages"],
            "p50": percentile_from_buckets(pooled["buckets"],
                                           pooled["messages"], 50),
            "p99": percentile_from_buckets(pooled["buckets"],
                                           pooled["messages"], 99),
            "p99_9": percentile_from_buckets(pooled["buckets"],
                                             pooled["messages"], 99.9),
        },
        "histogram_buckets": pooled["buckets"],
    }
