#!/usr/bin/env python3
"""Class A fidelity check of a recovered Ambre event (invariants I2 / I4).

The official verifier (host/verify/edr_verify.py) maps every class A point
onto the scenario by ABSOLUTE timestamp: it takes the event's t0_ts_us from
the journal and looks the scenario row up at t0 - rel_time_ms. That works for
the level-1 explorer, whose images carry scenario-relative stamps. It cannot
work for the real process chain, for two reasons measured on this tree:

1. edr-capture timestamps samples with ambre_port_now_us() (CLOCK_MONOTONIC),
   so the journal's t0_ts_us is a machine uptime, not a scenario offset;
2. edr-replay never emits the wheel-speed signals (10..13), so the class A
   speed of every point is 0 while the scenario carries the real speed.

Both are gaps in target/ (other agents' files); they are reported, not
papered over. This module therefore does the comparison itself, on the
fields the chain really carries, and finds the alignment by value: the
oldest class A point is matched against the scenario rows, which pins the
scenario-relative T0, and every other point must then match too.

"Identical to the injected data" is checked with the same tolerance the
official verifier documents (one least-significant unit per field), plus a
one-millisecond row slop because rel_time_ms is an integer.

English only, Python 3 standard library only.
"""

from __future__ import annotations

import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
for _sub in (("host", "verify"), ("host", "mcap")):
    _path = os.path.join(_ROOT, *_sub)
    if _path not in sys.path:
        sys.path.insert(0, _path)

import edr_verify as official            # noqa: E402  (reuse, never duplicate)
from ambre_mcap_writer import McapReader  # noqa: E402

# The fields the real chain carries. speed_kmh is left out on purpose: the
# replay does not emit wheel speeds, so the class A speed is always 0 here
# (documented gap, see the module header). Everything else is compared.
CARRIED_FIELDS = tuple(item for item in official.CLASS_A_COMPARE
                       if item[0] != "speed_kmh")
SKIPPED_FIELDS = ("speed_kmh",)

TOPIC_PRECRASH = "/edr/a/precrash"
ROW_SLOP_MS = 4       # strict window around the nominal instant
MAX_LAG_MS = 250      # widest common harness lag still accepted, measured


def point_matches_row(point: dict, row: dict) -> bool:
    """True when every carried field of the point equals the scenario row."""
    for field, scen_field, scale, tolerance in CARRIED_FIELDS:
        if field not in point or scen_field not in row:
            return False
        if abs(float(point[field]) - float(row[scen_field]) * scale) > tolerance:
            return False
    return True


def read_points(mcap_path: str):
    """(points sorted by age, metadata) of one exported event."""
    reader = McapReader(mcap_path)
    by_topic = reader.messages_by_topic()
    points = [json.loads(item["data"].decode("utf-8"))
              for item in by_topic.get(TOPIC_PRECRASH, [])]
    points.sort(key=lambda p: -int(p.get("rel_time_ms", 0)))
    return points, reader.metadata


def _row_slops(slop_ms: int = ROW_SLOP_MS):
    """Row offsets tried around the nominal instant, nearest first.

    rel_time_ms is a truncated integer and the alignment of T0 is itself
    known only to about a millisecond, while the class A accelerations are
    the last 1 kHz sample seen: during the impact ramp they change by a few
    thousand mm/s^2 per millisecond. A four-millisecond row slop absorbs
    that without loosening the field tolerance, and the 1 kHz acceleration
    noise still pins the matching row to a single millisecond.
    """
    slops = [0]
    for step in range(1, slop_ms + 1):
        slops += [-step * 1000, step * 1000]
    return slops


def _matches_at_t0(points, rows, t0_us, slop_ms=ROW_SLOP_MS):
    """Score one alignment: how many points match the scenario when the
    scenario-relative T0 sits at t0_us.

    A point older than the first injected sample has no counterpart at all
    (the class A ring reaches 5.5 s back, a short scenario does not): it is
    counted apart, never silently accepted as a match.
    """
    matched, exact, outside, worst, problems = 0, 0, 0, 0, []
    for point in points:
        want = t0_us - int(point["rel_time_ms"]) * 1000
        if want < 0:
            outside += 1
            continue
        for index, slop in enumerate(_row_slops(slop_ms)):
            row = rows.get(want + slop)
            if row is not None and point_matches_row(point, row):
                matched += 1
                exact += (index == 0)
                worst = max(worst, abs(slop) // 1000)
                break
        else:
            problems.append("rel_time_ms=%s matches no injected row within "
                            "%d ms of %d us" % (point["rel_time_ms"],
                                                slop_ms, want))
    return {"matched": matched, "exact": exact, "outside": outside,
            "worst_slop_ms": worst, "problems": problems}


def _better(score, best):
    """Rank two alignments: most points matched, then most exact rows."""
    if best is None:
        return True
    key = (score["matched"], score["exact"], -score["worst_slop_ms"])
    old = (best[0]["matched"], best[0]["exact"], -best[0]["worst_slop_ms"])
    return key > old


def _best_alignment(points, rows, crash_us, search_ms, slop_ms):
    """Search the scenario-relative T0 that explains the most points."""
    best = None
    for offset_ms in range(-search_ms, search_ms + 1):
        score = _matches_at_t0(points, rows, crash_us + offset_ms * 1000,
                               slop_ms)
        if _better(score, best):
            best = (score, offset_ms)
        if not score["problems"] and score["matched"] > 0:
            break                        # every in-scenario point matched
    return best


def _fill(result, best, total):
    """Turn one alignment score into the returned report."""
    score, offset_ms = best
    result.update(matched=score["matched"], matched_exact_ms=score["exact"],
                  outside_scenario=score["outside"],
                  worst_row_slop_ms=score["worst_slop_ms"],
                  problems=score["problems"][:5], t0_offset_ms=offset_ms,
                  ok=(score["matched"] + score["outside"] == total
                      and score["matched"] > 0))
    return result


def _find_value_rows(point, rows):
    """Every scenario row whose values equal this point (whole-scenario)."""
    return [t_us for t_us, row in rows.items()
            if point_matches_row(point, row)]


def _explain_points(points, rows, t0_us, max_lag_ms):
    """Explain every point: strict, lagged, drifted, or unexplained.

    Three cases, kept apart and measured:

    * strict  -- the point equals the injected row at its nominal instant
      (within the documented row slop);
    * lagged  -- it equals a row inside the harness-lag window: the state a
      class A point carries trails its own timestamp by the processing delay
      of a loaded PC bench;
    * drifted -- it equals an injected row somewhere else in the scenario, and
      only there. The value is then provably genuine (it was injected) but its
      timestamp pairing is wrong, which happens when the producer stalls: the
      capture stamps samples on arrival, so a stalled replay pairs old values
      with new instants. Measured, reported, never silent.

    A point whose values match no injected row anywhere is a failure: that is
    what corruption or a fabricated record would look like.
    Returns (mapping, problems) where mapping is {rel_time_ms: row_us}.
    """
    mapping, problems = {}, []
    for point in points:
        rel = int(point["rel_time_ms"])
        want = t0_us - rel * 1000
        if want < 0:
            mapping[rel] = None              # older than the scenario
            continue
        hit = _first_match(point, rows, want, _row_slops(ROW_SLOP_MS))
        if hit is None:
            hit = _first_match(point, rows, want,
                               [lag * 1000 for lag
                                in range(-max_lag_ms, max_lag_ms + 1)])
        if hit is None:
            found = _find_value_rows(point, rows)
            hit = min(found, key=lambda t: abs(t - want)) if found else None
        if hit is None:
            problems.append("rel_time_ms=%d matches no injected row at all "
                            "(nominal %d us)" % (rel, want))
            continue
        mapping[rel] = hit
    return mapping, problems


def _first_match(point, rows, want, slops):
    """First row offset in `slops` whose values equal the point."""
    for slop in slops:
        row = rows.get(want + slop)
        if row is not None and point_matches_row(point, row):
            return want + slop
    return None


def _mapping_stats(mapping, t0_us):
    """Strict / lagged / drifted counts, worst lag and worst drift (ms)."""
    strict = lagged = drifted = outside = 0
    worst_lag = worst_drift = 0
    for rel, row_us in mapping.items():
        if row_us is None:
            outside += 1
            continue
        delta_ms = (row_us - (t0_us - rel * 1000)) // 1000
        if abs(delta_ms) <= ROW_SLOP_MS:
            strict += 1
            worst_lag = max(worst_lag, abs(delta_ms))
        elif abs(delta_ms) <= MAX_LAG_MS:
            lagged += 1
            worst_lag = max(worst_lag, abs(delta_ms))
        else:
            drifted += 1
            worst_drift = max(worst_drift, abs(delta_ms))
    return {"strict": strict, "lagged": lagged, "drifted": drifted,
            "outside_scenario": outside, "worst_lag_ms": worst_lag,
            "worst_drift_ms": worst_drift}


def _ordered(mapping) -> bool:
    """Older points must map to older rows: the class A order is invariant."""
    known = [(rel, row) for rel, row in mapping.items() if row is not None]
    known.sort(key=lambda item: -item[0])
    return all(known[i][1] <= known[i + 1][1] for i in range(len(known) - 1))


def _empty_result(points, skipped):
    """The report shape of check_class_a, before any comparison."""
    return {"ok": False, "ok_strict": False, "matched": 0,
            "matched_exact_ms": 0, "points_strict": 0, "points_lagged": 0,
            "points_drifted": 0, "outside_scenario": 0,
            "worst_row_slop_ms": 0, "skew_ms": None, "worst_drift_ms": None,
            "lag_ms_per_point": {}, "total": len(points),
            "t0_offset_ms": None, "problems": [], "skipped": list(skipped)}


def _explain_pass(result, points, rows, t0_us):
    """Second pass: explain the points the strict alignment left over."""
    mapping, problems = _explain_points(points, rows, t0_us, MAX_LAG_MS)
    stats = _mapping_stats(mapping, t0_us)
    result["points_strict"] = stats["strict"]
    result["points_lagged"] = stats["lagged"]
    result["points_drifted"] = stats["drifted"]
    result["outside_scenario"] = stats["outside_scenario"]
    result["skew_ms"] = stats["worst_lag_ms"] or None
    result["worst_drift_ms"] = stats["worst_drift_ms"] or None
    result["matched"] = stats["strict"] + stats["lagged"] + stats["drifted"]
    result["lag_ms_per_point"] = {
        str(rel): (row - (t0_us - rel * 1000)) // 1000
        for rel, row in mapping.items() if row is not None}
    if problems:
        result["problems"] = problems[:5]
        return result
    if not _ordered(mapping):
        result["problems"] = ["the class A points are not in the order of the "
                              "injected rows: %s" % mapping]
        return result
    result["ok"] = result["matched"] > 0
    if stats["drifted"] or stats["lagged"]:
        result["problems"] = [
            "%d point(s) needed a harness lag (<= %d ms) and %d a producer "
            "stall drift (up to %d ms); every value is an injected one"
            % (stats["lagged"], MAX_LAG_MS, stats["drifted"],
               stats["worst_drift_ms"])]
    return result


def check_class_a(mcap_path: str, scenario_path: str, search_ms: int = 600):
    """Compare the exported class A points with the injected scenario.

    The scenario-relative T0 is found by value: every candidate instant around
    the impact is scored against the WHOLE set of points and the best score
    wins. Anchoring on a single point is not enough -- in the quiet part of a
    drive many rows are identical, so one point alone can be matched at the
    wrong place.

    Every point must then equal an injected row (the verifier's own 1-LSB
    tolerance per field), and the mapping must stay ordered. How far each point
    sits from its nominal instant is measured and reported: `worst_row_slop_ms`
    (strict), `skew_ms` (harness lag) and `worst_drift_ms` (producer stall).
    A point that matches no injected row anywhere fails the check. See
    _explain_points for the three cases and what each one means.
    """
    header, rows = official.load_scenario(scenario_path)
    crash_us = int(header.get("crash_at_ms", 0)) * 1000
    points, _meta = read_points(mcap_path)
    result = _empty_result(points, SKIPPED_FIELDS)
    if not points:
        result["problems"].append("the export carries no class A point")
        return result

    score, offset_ms = _best_alignment(points, rows, crash_us, search_ms,
                                       ROW_SLOP_MS)
    t0_us = crash_us + offset_ms * 1000
    _fill(result, (score, offset_ms), len(points))
    result["ok_strict"] = result["ok"]
    if result["ok"]:
        result["points_strict"] = score["matched"]
        result["outside_scenario"] = score["outside"]
        return result
    return _explain_pass(result, points, rows, t0_us)


def non_monotonic_topics(mcap_path: str):
    """Topics whose log time goes backwards, with the offending pairs.

    Used to tell an export-ordering defect apart from a cut-induced one: the
    gap records of several signals are appended in detection order, so the
    /edr/meta/gaps channel can go backwards even in a run with no cut at all.
    """
    reader = McapReader(mcap_path)
    out = {}
    for topic, messages in reader.messages_by_topic().items():
        times = [item["log_time"] for item in messages]
        bad = [(times[i - 1], times[i]) for i in range(1, len(times))
               if times[i] < times[i - 1]]
        if bad:
            out[topic] = bad[:3]
    return out
