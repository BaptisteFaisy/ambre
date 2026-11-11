#!/usr/bin/env python3
"""Cross-check the Python trigger twin against the C reference decisions (.dec).

Modes:
  --vectors <in.vec> --c-output <c.dec> [--verbose]
      Run the Python engine over <in.vec>, compare its `D ...` lines to the C
      `.dec`, and report divergences. Exit 0 if none, 1 otherwise.

  --self
      Standalone smoke test that needs no C binary: generate a small vector set,
      run it twice for determinism, and assert the three DEC-08 boundary cases.
"""

import argparse
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ambre_trigger as at  # noqa: E402
import gen_vectors  # noqa: E402

# Field names for the tokens AFTER the leading "D" marker.
FIELD_NAMES = [
    "id", "i", "fired", "end", "locked", "reason", "t0_us", "event_number",
    "time_since_first_ms", "dv_long", "dv_lat", "dv_since_t0_long",
    "dv_since_t0_lat", "max_dv_long", "max_dv_lat", "max_dv_long_at_ms",
    "max_dv_lat_at_ms", "win_long", "win_lat",
]


def read_dec_lines(path: str) -> list:
    """Read decision lines (`D ...`) from a C .dec file, skipping comments."""
    lines = []
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            s = raw.rstrip("\r\n")
            if not s.strip() or s.lstrip().startswith("#"):
                continue
            if s.startswith("D ") or s == "D":
                lines.append(s)
    return lines


def _diff_fields(py_line: str, c_line: str) -> list:
    """Return a list of (field_name, c_value, py_value) for differing columns."""
    pt = py_line.split()
    ct = c_line.split()
    diffs = []
    n = max(len(pt), len(ct))
    for k in range(1, n):  # skip token 0 == "D"
        pv = pt[k] if k < len(pt) else "<missing>"
        cv = ct[k] if k < len(ct) else "<missing>"
        if pv != cv:
            name = FIELD_NAMES[k - 1] if (k - 1) < len(FIELD_NAMES) else f"col{k}"
            diffs.append((name, cv, pv))
    return diffs


def compare(py_lines: list, c_lines: list, verbose: bool) -> tuple:
    """Compare Python vs C decision lines. Returns (n_samples, n_div, details)."""
    n_div = 0
    details = []
    n = max(len(py_lines), len(c_lines))
    for i in range(n):
        py = py_lines[i] if i < len(py_lines) else None
        c = c_lines[i] if i < len(c_lines) else None
        if py == c:
            continue
        n_div += 1
        if py is None:
            details.append((i, "line-count", "<missing>", "C extra: " + (c or "")))
            continue
        if c is None:
            details.append((i, "line-count", "Py extra: " + py, "<missing>"))
            continue
        for name, cv, pv in _diff_fields(py, c):
            details.append((i, name, cv, pv))
    n_samples = len(py_lines)
    if verbose:
        for i in range(min(len(py_lines), len(c_lines))):
            if py_lines[i] != c_lines[i]:
                print(f"  line {i}:\n    C : {c_lines[i]}\n    PY: {py_lines[i]}")
    return n_samples, n_div, details


def _print_report(n_scen, n_samples, n_div, details) -> None:
    print(f"scenarios:        {n_scen}")
    print(f"samples compared: {n_samples}")
    print(f"divergences:      {n_div}")
    shown = 0
    for idx, field, cval, pval in details:
        if shown >= 10:
            break
        print(f"  [div {shown + 1}] line={idx} field={field} C={cval} PY={pval}")
        shown += 1


def run_compare(vec_path: str, dec_path: str, verbose: bool) -> int:
    """Comparison mode entry point."""
    scenarios = at.parse_vec(vec_path)
    py_lines = at.dec_lines_for_scenarios(scenarios)
    if not os.path.exists(dec_path):
        print(f"error: C output not found: {dec_path}", file=sys.stderr)
        return 2
    c_lines = read_dec_lines(dec_path)
    n_samples, n_div, details = compare(py_lines, c_lines, verbose)
    _print_report(len(scenarios), n_samples, n_div, details)
    if n_div == 0 and len(py_lines) == len(c_lines):
        print("RESULT: OK (byte-identical decisions)")
        return 0
    print("RESULT: DIVERGENCE")
    return 1


def _check_dec08(scenarios: list) -> list:
    """Verify the three DEC-08 boundary scenarios (ids 1,2,3). Return failures."""
    failures = []
    by_id = {s.scen_id: s for s in scenarios}
    expect = {1: False, 2: True, 3: False}  # scen_id -> should any sample fire?
    want_dv = {1: 2194, 2: 2250, 3: 2250}   # exact cumulative delta-v (mm/s)
    for sid, want_fire in expect.items():
        if sid not in by_id:
            failures.append(f"DEC-08 scenario id={sid} missing")
            continue
        outs = at.run_scenario(by_id[sid])
        did_fire = any(o.fired for o in outs)
        if did_fire != want_fire:
            failures.append(f"DEC-08 id={sid}: fired={did_fire} expected={want_fire}")
        if outs and outs[-1].dv_long_mm_s != want_dv[sid]:
            failures.append(f"DEC-08 id={sid}: dv={outs[-1].dv_long_mm_s} "
                            f"expected={want_dv[sid]}")
    # Case 3 must still fire in WINDOW mode: that is what ANCHORED is for.
    if 3 in by_id:
        window_cfg = at.TrigConfig(mode=at.AMBRE_TRIG_MODE_WINDOW)
        w_outs = at.run_scenario(at.Scenario(3, window_cfg, by_id[3].samples))
        if not any(o.fired for o in w_outs):
            failures.append("DEC-08 id=3: WINDOW mode should fire (8.1 km/h "
                            "reached in 151 ms is only excluded by ANCHORED)")
    return failures


def run_self() -> int:
    """Standalone self-test: determinism + DEC-08 + trigger coverage."""
    fd, vec_path = tempfile.mkstemp(suffix=".vec", prefix="ambre_self_")
    os.close(fd)
    try:
        total, triggered = gen_vectors.generate_vectors(vec_path, 40, seed=12345, mode=-1)
        scenarios = at.parse_vec(vec_path)
        lines_a = at.dec_lines_for_scenarios(scenarios)
        lines_b = at.dec_lines_for_scenarios(scenarios)
        failures = []
        if lines_a != lines_b:
            failures.append("determinism: two runs produced different decisions")
        failures.extend(_check_dec08(scenarios))
        n_fire = sum(1 for s in scenarios if any(o.fired for o in at.run_scenario(s)))
        if n_fire == 0:
            failures.append("no scenario fired (engine likely broken)")

        print(f"self: scenarios={total} generated, {triggered} expected-trigger")
        print(f"self: scenarios actually firing={n_fire}")
        print(f"self: decision lines={len(lines_a)}, determinism={'OK' if lines_a == lines_b else 'FAIL'}")
        if failures:
            for msg in failures:
                print(f"  FAIL: {msg}")
            print("RESULT: SELF-TEST FAILED")
            return 1
        print("RESULT: SELF-TEST OK")
        return 0
    finally:
        try:
            os.unlink(vec_path)
        except OSError:
            pass


def main() -> None:
    parser = argparse.ArgumentParser(description="Cross-check Python trigger vs C .dec")
    parser.add_argument("--vectors", help="Input .vec path")
    parser.add_argument("--c-output", help="C reference .dec path")
    parser.add_argument("--verbose", action="store_true", help="Print diverging lines")
    parser.add_argument("--self", action="store_true", help="Standalone self-test")
    args = parser.parse_args()

    if args.self:
        sys.exit(run_self())
    if not args.vectors or not args.c_output:
        parser.error("--vectors and --c-output are required (or use --self)")
    sys.exit(run_compare(args.vectors, args.c_output, args.verbose))


if __name__ == "__main__":
    main()
