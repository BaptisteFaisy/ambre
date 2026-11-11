#!/usr/bin/env python3
"""Statistics, scenario splitting and trace parsing for the latency campaign.

Pure helpers shared by host/latency/measure.py and tests/test_latency_py.py.
No third-party dependency: the campaign must run on a bare Python 3.

Percentile convention (documented in docs/CAMPAIGN-LATENCY.md): nearest rank,
    percentile(v, p) = sorted(v)[ceil(p/100 * n) - 1]
so the median is percentile(v, 50) and p99 is percentile(v, 99).  The same
convention as edr-replay's histogram (target/replay/edr_replay.c).
"""

from __future__ import annotations

import math

TRACE_TRIGGER = 1        # AMBRE_TRACE_TRIGGER: pulse received by the journal
TRACE_WRITE_SUBMIT = 2  # AMBRE_TRACE_WRITE_SUBMIT: emergency write handed over
TRACE_FLUSH_ACK = 3     # AMBRE_TRACE_FLUSH_ACK: flush acknowledged, "acquired"
TRACE_SEAL = 4          # AMBRE_TRACE_SEAL: event sealed
TRACE_RECOVER = 5       # AMBRE_TRACE_RECOVER: recovery started

# Trace bucket ceilings in microseconds, used for the ASCII histograms.
FREEZE_BUCKETS_US = (100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000,
                     100000, 250000, 1000000)
TRIGGER_BUCKETS_US = (50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000,
                      50000, 100000)


def percentile(values, pct):
    """Nearest-rank percentile of a non-empty list; pct in percent (0 < p <= 100)."""
    if not values:
        return None
    ordered = sorted(values)
    rank = math.ceil((pct / 100.0) * len(ordered))
    rank = max(1, min(rank, len(ordered)))
    return ordered[rank - 1]


def summarize(values):
    """count/min/median/p99/p99.9/max of a list of numbers (None if empty)."""
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "min": min(values),
        "median": percentile(values, 50),
        "p99": percentile(values, 99),
        "p99_9": percentile(values, 99.9),
        "max": max(values),
        "mean": round(sum(values) / len(values), 2),
    }


def histogram(values, buckets):
    """Count per bucket; the last bucket is the overflow."""
    counts = [0] * (len(buckets) + 1)
    for value in values:
        for index, ceiling in enumerate(buckets):
            if value <= ceiling:
                counts[index] += 1
                break
        else:
            counts[-1] += 1
    return counts


def render_histogram(values, buckets, unit="us", title=""):
    """ASCII histogram, one line per bucket, with the share in percent."""
    counts = histogram(values, buckets)
    total = max(1, len(values))
    lines = []
    if title:
        lines.append(title)
    lines.append(f"  {'<= ' + str(buckets[0]) + ' ' + unit:>18} | "
                 f"{counts[0]:>6} {100.0 * counts[0] / total:5.1f}%")
    for index in range(1, len(buckets)):
        label = f"<= {buckets[index]} {unit}"
        lines.append(f"  {label:>18} | {counts[index]:>6} "
                     f"{100.0 * counts[index] / total:5.1f}%")
    lines.append(f"  {'> ' + str(buckets[-1]) + ' ' + unit:>18} | "
                 f"{counts[-1]:>6} {100.0 * counts[-1] / total:5.1f}%")
    return "\n".join(lines)


# --------------------------------------------------------------- scenarios --

def parse_scenario(path):
    """(header dict, rows list) of a docs/FORMAT.md section 11 scenario file."""
    header = {}
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            text = line.strip()
            if not text or text.startswith("#"):
                continue
            if "=" in text:
                key, _, value = text.partition("=")
                header[key] = value
                continue
            fields = text.split()
            if len(fields) < 3:
                continue
            rows.append([int(x) for x in fields[:9]])
    return header, rows


def find_shocks(rows, threshold_mm_s2=2000, max_gap_rows=3):
    """Index ranges of the shocks: runs of rows above the threshold."""
    shocks = []
    start = None
    last_hot = -10 ** 9
    for index, row in enumerate(rows):
        if abs(row[1]) >= threshold_mm_s2:
            if start is None:
                start = index
            last_hot = index
        elif start is not None and index - last_hot > max_gap_rows:
            shocks.append((start, last_hot))
            start = None
    if start is not None:
        shocks.append((start, last_hot))
    return shocks


def quiet_row(ts_us, speed_x4):
    """One quiet driving row (same shape as the fixed generator's rows)."""
    return [ts_us, 0, 0, speed_x4, 0, 0, 800, 0, 0]


def build_chunk(rows, shock, lead_rows=10, tail_rows=200, period_us=1000):
    """Rows of one campaign chunk: quiet lead, the shock, quiet tail.

    The timestamps are normalised to the nominal sample period, re-based on
    zero: edr-replay paces by frame index and rejects any frame whose row
    span reaches 10 ms (ACQ-01), so the spacing must stay exactly nominal.
    The capture evaluates the trigger on its own monotonic clock, so only the
    row order and the spacing matter.
    """
    first, last = shock
    start = max(0, first - lead_rows)
    head = [list(row) for row in rows[start:first]]
    body = [list(row) for row in rows[first:last + 1]]
    speed = body[-1][3] if body else (head[-1][3] if head else 0)
    tail = [quiet_row(0, speed) for _ in range(tail_rows)]
    chunk = head + body + tail
    for index, row in enumerate(chunk):
        row[0] = index * period_us
    return chunk


def write_scenario(path, chunk, name="latency-chunk"):
    """Write a chunk as a scenario file the C loader accepts."""
    header = [f"# ambre scenario v1", f"name={name}", "seed=1", "rate_hz=1000",
              f"duration_ms={chunk[-1][0] // 1000 + 1}", "crash_at_ms=0",
              "# t_us a_long_mm_s2 a_lat_mm_s2 speed_kmh_x4 throttle brake rpm "
              "steer_x2 ext_flags"]
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(header) + "\n")
        for row in chunk:
            handle.write(" ".join(str(value) for value in row) + "\n")


# ------------------------------------------------------------------ traces --

def parse_trace(path):
    """{event_id: {trace_point: timestamp_us}} from an AMBRE_TRACE file."""
    events = {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                fields = line.split()
                if len(fields) < 3:
                    continue
                ts = int(fields[0])
                event_id = int(fields[1])
                point = int(fields[2])
                events.setdefault(event_id, {})[point] = ts
    except FileNotFoundError:
        return {}
    return events
