#!/usr/bin/env python3
"""energy_model.py — hold-up energy model of the Ambre recorder (spec 12.4).

The extreme-level item 12.4 of the cahier des charges asks for an energy
reserve model: a "voltage" input that decays like a discharging capacitor,
showing that the freeze ends before the shutdown threshold and which
capacitance is required. The reference given by the specification is
"at 5 W, 20 ms need 0.1 J".

MODEL (documented, RC-like)
---------------------------
The recorder is fed by a hold-up capacitor C charged at V0 through the
supply impedance R (wiring + source output impedance). The supply is cut at
t = 0; from then on the capacitor alone feeds the recorder, which draws a
constant power P during the freeze (the recorder's own consumption is
negligible next to it).

    I      = P / u                  load current (constant power)
    V_C    = u + I * R              capacitor voltage (u = load voltage)
    C dV_C/dt = -I                  capacitor discharge
 =>  du/dt = -P * u / (C * (u^2 - P*R))

The model is integrated with RK4 (step 1 us). The singularity at u^2 = P*R is
the well-known collapse of a constant-power load: the run needs u > sqrt(P*R)
all the way down, which the tool checks and reports.

Energy accounting (the tool verifies it numerically):
    E0    = C * V0^2 / 2                     stored at the cut
    E_load= P * t                            taken by the recorder
    E_R   = integral(I^2 R dt)               lost in R
    E_end = C * V_C(t)^2 / 2                 left in the capacitor

HONESTY: the freeze power and the freeze delay are NOT measured (no QNX
target yet). Every delay printed by this tool is a TARGET, marked as such,
and every parameter is overridable on the command line.

Usage:
    python3 host/energy/energy_model.py
    python3 host/energy/energy_model.py --power 5 --delays 5,10,20,50 \
        --json build/energy.json --svg build/energy.svg
"""

import argparse
import json
import math
import sys

DT = 1e-6                      # integration step: 1 us
REFERENCE_POWER_W = 5.0        # cahier des charges 12.4: "at 5 W, 20 ms"
REFERENCE_DELAY_MS = 20.0
MEASURED = False               # the freeze power/delay are targets, not data


def discharge_derivative(u, cap, power, resistance):
    """du/dt for the constant-power load fed by a hold-up capacitor."""
    denom = u * u - power * resistance
    if denom <= 0.0 or u <= 0.0:
        return -1e9
    return -power * u / (cap * denom)


def rk4_step(u, dt, cap, power, resistance):
    """One RK4 step of the discharge equation."""
    k1 = discharge_derivative(u, cap, power, resistance)
    k2 = discharge_derivative(u + 0.5 * dt * k1, cap, power, resistance)
    k3 = discharge_derivative(u + 0.5 * dt * k2, cap, power, resistance)
    k4 = discharge_derivative(u + dt * k3, cap, power, resistance)
    return u + (dt / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4)


def integrate(cap, v0, power, resistance, duration_s, samples=200):
    """Return (times, load voltages, capacitor voltages, energy balance).

    The load voltage at t=0 is v0; the capacitor therefore starts at
    V_C(0) = v0 + P*R/v0 (the load current already flows through R).
    Integration stops early if the constant-power load collapses, i.e. when
    u reaches sqrt(P*R): below that point no steady state exists.
    """
    steps = max(1, int(duration_s / DT))
    step_dt = duration_s / steps
    every = max(1, steps // samples)
    times, loads, caps = [0.0], [v0], [v0 + power * resistance / v0]
    u = v0
    loss = 0.0
    collapsed = False
    floor_v = math.sqrt(power * resistance)
    prev_i = power / v0 if v0 > 0.0 else 0.0
    for index in range(1, steps + 1):
        u_next = rk4_step(u, step_dt, cap, power, resistance)
        if u_next <= floor_v or u_next != u_next:   # collapse or NaN
            u_next = floor_v
            collapsed = True
        curr_i = power / u if u > 0.0 else prev_i
        loss += 0.5 * (prev_i * prev_i + curr_i * curr_i) * resistance * step_dt
        prev_i = curr_i
        u = u_next
        if index % every == 0 or index == steps or collapsed:
            v_cap = u + (power * resistance / u) if u > 0.0 else 0.0
            times.append(index * step_dt)
            loads.append(u)
            caps.append(v_cap)
        if collapsed:
            break
    v_c0 = v0 + power * resistance / v0
    e0 = 0.5 * cap * v_c0 * v_c0
    e_end = 0.5 * cap * caps[-1] * caps[-1]
    e_load = power * times[-1]
    balance = e0 - e_load - loss - e_end
    return times, loads, caps, {"e0": e0, "e_load": e_load, "e_r": loss,
                                 "e_end": e_end, "balance": balance,
                                 "collapsed": collapsed,
                                 "duration_s": times[-1]}


def voltage_at(cap, v0, power, resistance, duration_s):
    """Load voltage at the end of the freeze (no curve kept).

    Returns 0.0 when the constant-power load collapses (u <= sqrt(P*R)).
    """
    steps = max(1, int(duration_s / DT))
    step_dt = duration_s / steps
    floor_v = math.sqrt(power * resistance)
    u = v0
    for _ in range(steps):
        u = rk4_step(u, step_dt, cap, power, resistance)
        if u <= floor_v or u != u:
            return 0.0
    return u


def min_capacity(v0, power, resistance, v_shutdown, duration_s):
    """Smallest capacitance keeping the load above the shutdown threshold."""
    lo, hi = 1e-7, 10.0
    if voltage_at(hi, v0, power, resistance, duration_s) < v_shutdown:
        return None
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        if voltage_at(mid, v0, power, resistance, duration_s) >= v_shutdown:
            hi = mid
        else:
            lo = mid
    return hi


def energy_only_capacity(v0, power, resistance, v_shutdown, duration_s):
    """Capacity ignoring the series loss: C = 2 P t / (V0^2 - Vsd^2)."""
    usable = v0 * v0 - v_shutdown * v_shutdown
    if usable <= 0.0:
        return None
    return 2.0 * power * duration_s / usable


def reference_energy(power, duration_s):
    """The energy the specification quotes: P * t."""
    return power * duration_s


def build_case(v0, power, resistance, v_shutdown, delay_ms):
    """One (power, delay) case: energies, minimum capacity, margins."""
    duration = delay_ms / 1000.0
    c_min = min_capacity(v0, power, resistance, v_shutdown, duration)
    c_energy = energy_only_capacity(v0, power, resistance, v_shutdown, duration)
    case = {
        "delay_ms": delay_ms,
        "status": "target, not measured" if not MEASURED else "measured",
        "power_w": power,
        "energy_needed_j": reference_energy(power, duration),
        "c_min_mf": None if c_min is None else c_min * 1e3,
        "c_energy_only_mf": None if c_energy is None else c_energy * 1e3,
        "collapse_v": math.sqrt(power * resistance),
    }
    if c_min is None:
        case["note"] = "no capacity keeps the load above the threshold"
        return case
    times, loads, caps, energy = integrate(c_min, v0, power, resistance, duration)
    case["v_end_at_c_min"] = loads[-1]
    case["v_start"] = loads[0]
    case["energy"] = energy
    case["energy_balance_rel"] = abs(energy["balance"]) / max(energy["e0"], 1e-12)
    return case


def margin_at(cap, v0, power, resistance, v_shutdown, delay_ms):
    """Voltage and energy margin at the end of the freeze for one capacity."""
    duration = delay_ms / 1000.0
    times, loads, caps, energy = integrate(cap, v0, power, resistance, duration)
    e_usable = 0.5 * cap * (v0 * v0 - v_shutdown * v_shutdown)
    return {
        "delay_ms": delay_ms,
        "collapsed": energy["collapsed"],
        "v_end": loads[-1],
        "v_margin": loads[-1] - v_shutdown,
        "v_margin_pct": 100.0 * (loads[-1] - v_shutdown) / v_shutdown,
        "energy_needed_j": power * duration,
        "energy_usable_j": e_usable,
        "energy_margin_pct": (100.0 * (e_usable - power * duration)
                              / (power * duration) if power > 0 else 0.0),
        "energy_loss_r_j": energy["e_r"],
    }


def ascii_plot(series, v_shutdown, width=72, height=18):
    """Hand-drawn ASCII plot of the voltage curves (no external library).

    Each series is drawn with its own marker ('1', '2', ...); the shutdown
    threshold appears as a dashed row.
    """
    v_max = max(max(vals) for _, vals in series)
    v_min = v_shutdown * 0.9
    grid = [[" "] * width for _ in range(height)]

    def put(value, col, ch):
        row = int(round((v_max - value) / (v_max - v_min) * (height - 1)))
        c = int(round(col))
        if 0 <= row < height and 0 <= c < width:
            grid[row][c] = ch

    row_sd = int(round((v_max - v_shutdown) / (v_max - v_min) * (height - 1)))
    for col in range(width):
        if grid[row_sd][col] == " ":
            grid[row_sd][col] = "-"
    for index, (_, vals) in enumerate(series):
        marker = str(index + 1)
        count = len(vals)
        for i, value in enumerate(vals):
            put(value, i * (width - 1) / max(1, count - 1), marker)
    rows = ["%6.2f V |%s" % (v_max - (v_max - v_min) * r / (height - 1),
                             "".join(grid[r])) for r in range(height)]
    rows.append("         +" + "-" * width)
    legend = "  ".join("%s=%s" % (str(i + 1), label)
                       for i, (label, _) in enumerate(series))
    rows.append("          0 s -> end of freeze   [%s]" % legend)
    rows.append("          dashed row = shutdown threshold %.2f V" % v_shutdown)
    return "\n".join(rows)


def svg_plot(series, v_shutdown, path, width=760, height=420):
    """Write a standalone SVG (inline styles, no external resource)."""
    v_max = max(max(vals) for _, vals in series)
    v_min = min(v_shutdown, v_max * 0.2)
    left, right, top, bottom = 70, 20, 20, 50
    plot_w = width - left - right
    plot_h = height - top - bottom
    parts = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
             'viewBox="0 0 %d %d" font-family="monospace">' % (width, height, width, height),
             '<rect width="100%" height="100%" fill="#ffffff"/>',
             '<text x="%d" y="16" font-size="13">Ambre hold-up energy - '
             'load voltage during the freeze (target, not measured)</text>' % left]
    for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
        value = v_max - frac * (v_max - v_min)
        y = top + plot_h * frac
        parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#dddddd"/>'
                     % (left, y, left + plot_w, y))
        parts.append('<text x="4" y="%.1f" font-size="11">%.2f</text>' % (y + 4, value))
    y_sd = top + plot_h * (v_max - v_shutdown) / (v_max - v_min)
    parts.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#c0392b" '
                 'stroke-dasharray="6,4"/>' % (left, y_sd, left + plot_w, y_sd))
    parts.append('<text x="%d" y="%.1f" font-size="11" fill="#c0392b">'
                 'shutdown threshold %.2f V</text>' % (left + 6, y_sd - 6, v_shutdown))
    colours = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd"]
    for index, (label, vals) in enumerate(series):
        count = len(vals)
        points = []
        for i, value in enumerate(vals):
            x = left + plot_w * i / (count - 1)
            y = top + plot_h * (v_max - value) / (v_max - v_min)
            points.append("%.1f,%.1f" % (x, y))
        parts.append('<polyline fill="none" stroke="%s" stroke-width="2" '
                     'points="%s"/>' % (colours[index % len(colours)], " ".join(points)))
        parts.append('<text x="%d" y="%d" font-size="11" fill="%s">%s</text>'
                     % (left + 8, top + 14 + 14 * index,
                        colours[index % len(colours)], label))
    parts.append('<text x="%d" y="%d" font-size="11">0 s</text>' % (left, height - 12))
    parts.append('<text x="%d" y="%d" font-size="11" text-anchor="end">'
                 'end of freeze</text>' % (left + plot_w, height - 12))
    parts.append("</svg>")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(parts) + "\n")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--v0", type=float, default=12.6,
                        help="capacitor voltage at the cut (V)")
    parser.add_argument("--v-shutdown", type=float, default=9.0,
                        help="shutdown threshold of the recorder (V)")
    parser.add_argument("--r", type=float, default=0.2,
                        help="series resistance of the supply path (Ohm)")
    parser.add_argument("--power", type=float, default=REFERENCE_POWER_W,
                        help="power drawn during the freeze (W, target)")
    parser.add_argument("--delays", default="5,10,20,50",
                        help="freeze delays to plot, in ms (targets)")
    parser.add_argument("--c-nom", type=float, default=None,
                        help="nominal hold-up capacitance in mF (default: "
                             "twice the minimum of the reference case)")
    parser.add_argument("--json", default=None, help="write the JSON report here")
    parser.add_argument("--svg", default=None, help="write the SVG curve here")
    parser.add_argument("--no-ascii", action="store_true",
                        help="do not print the ASCII curve")
    return parser


def print_assumptions(args, delays):
    print("Ambre hold-up energy model (spec 12.4)")
    print("  assumptions (all overridable):")
    print("    V0 = %.2f V   V_shutdown = %.2f V   R = %.3f Ohm"
          % (args.v0, args.v_shutdown, args.r))
    print("    freeze power = %.2f W  -- TARGET, NOT MEASURED (no QNX target)"
          % args.power)
    print("    freeze delays = %s ms -- TARGET, NOT MEASURED"
          % ",".join("%.0f" % d for d in delays))
    print("  model: C dV_C/dt = -I, I = P/u, V_C = u + I.R,")
    print("          => du/dt = -P u / (C (u^2 - P R)), RK4, dt = 1 us")
    print("")


def print_reference(args, reference):
    print("  reference of the specification (12.4): %.1f W x %.0f ms = %.6f J"
          % (args.power, REFERENCE_DELAY_MS, reference))
    print("  -> the cahier's 0.1 J is reproduced by the computation above "
          "(P x t).")
    print("")


def print_cases(cases):
    print("  %-9s %-22s %10s %10s %10s %8s" %
          ("delay", "status", "E (J)", "C_min (mF)", "C_energy (mF)", "V_end"))
    for case in cases:
        print("  %-9.0f %-22s %10.4f %10s %10s %8s" %
              (case["delay_ms"], case["status"], case["energy_needed_j"],
               "%.3f" % case["c_min_mf"] if case["c_min_mf"] else "n/a",
               "%.3f" % case["c_energy_only_mf"] if case["c_energy_only_mf"] else "n/a",
               "%.3f" % case["v_end_at_c_min"] if case["c_min_mf"] else "n/a"))
    print("")


def nominal_capacitance(args, cases):
    longest = max(cases, key=lambda c: c["delay_ms"])
    if args.c_nom:
        return args.c_nom / 1e3
    if longest["c_min_mf"]:
        return 2.0 * longest["c_min_mf"] / 1e3
    return 1e-3


def print_margins(args, delays, c_nom):
    print("  margin at the nominal hold-up capacitance C = %.3f mF"
          " (default: twice the minimum of the longest delay)" % (c_nom * 1e3))
    print("  %-9s %-22s %10s %10s %12s %12s %10s" %
          ("delay", "status", "V_end (V)", "margin (V)", "margin (%)",
           "E usable (J)", "state"))
    margins = []
    for delay in delays:
        margin = margin_at(c_nom, args.v0, args.power, args.r, args.v_shutdown,
                           delay)
        margins.append(margin)
        print("  %-9.0f %-22s %10.3f %10.3f %12.2f %12.4f %10s" %
              (delay, "target, not measured", margin["v_end"],
               margin["v_margin"], margin["v_margin_pct"],
               margin["energy_usable_j"],
               "COLLAPSED" if margin["collapsed"] else "ok"))
    print("")
    return margins


def print_power_grid(args, delays, power_grid):
    print("  minimum capacitance per power (mF), delays as columns")
    print("  %-8s" % "power (W)" + "".join("%10.0f ms" % d for d in delays))
    grid = {}
    for power in power_grid:
        row = "  %-8.1f" % power
        for delay in delays:
            cap = min_capacity(args.v0, power, args.r, args.v_shutdown,
                               delay / 1000.0)
            grid["%gW/%gms" % (power, delay)] = cap
            row += "%11s" % ("%.3f" % (cap * 1e3) if cap else "n/a")
        print(row)
    print("")
    return grid


def print_energy_balance(args, delays, c_nom):
    series = []
    collapse_v = math.sqrt(args.power * args.r)
    for delay in delays:
        times, loads, _caps, energy = integrate(c_nom, args.v0, args.power,
                                                args.r, delay / 1000.0,
                                                samples=120)
        series.append(("%.0f ms" % delay, loads))
        if energy["collapsed"]:
            print("  energy balance at %.0f ms: COLLAPSED after %.3f ms "
                  "(load below sqrt(P*R) = %.2f V, no steady state)"
                  % (delay, times[-1] * 1e3, collapse_v))
            continue
        print("  energy balance at %.0f ms (C = %.3f mF): E0 = %.4f J, "
              "load = %.4f J, R = %.6f J, end = %.4f J, residual = %.2e J"
              % (delay, c_nom * 1e3, energy["e0"], energy["e_load"],
                 energy["e_r"], energy["e_end"], energy["balance"]))
    return series


def build_report(args, delays, c_nom, reference, cases, margins, grid):
    return {
        "tool": "host/energy/energy_model.py",
        "spec_item": "12.4 (modele de reserve d'energie)",
        "measured": MEASURED,
        "status": "freeze power and freeze delay are TARGETS, not measurements",
        "parameters": {
            "v0_v": args.v0, "v_shutdown_v": args.v_shutdown,
            "r_ohm": args.r, "power_w": args.power,
            "delays_ms": delays, "c_nom_f": c_nom,
            "c_nom_basis": "2 x C_min(longest delay)" if args.c_nom is None
                           else "user supplied",
        },
        "model": {
            "ode": "du/dt = -P*u / (C*(u^2 - P*R))",
            "load": "I = P/u, V_C = u + I*R",
            "integration": "RK4, dt = 1 us",
            "collapse_v": math.sqrt(args.power * args.r),
        },
        "reference": {
            "power_w": args.power, "delay_ms": REFERENCE_DELAY_MS,
            "energy_j": reference,
            "cahier_claim_j": 0.1,
            "matches_cahier": abs(reference - 0.1) < 1e-9,
        },
        "cases": cases,
        "margins": margins,
        "c_min_grid_f": grid,
    }


def main(argv):
    args = build_parser().parse_args(argv)
    delays = [float(x) for x in args.delays.split(",") if x.strip()]
    power_grid = [args.power / 2.0, args.power, args.power * 2.0,
                  args.power * 4.0]

    print_assumptions(args, delays)
    reference = reference_energy(args.power, REFERENCE_DELAY_MS / 1000.0)
    print_reference(args, reference)

    cases = [build_case(args.v0, args.power, args.r, args.v_shutdown, d)
             for d in delays]
    print_cases(cases)

    c_nom = nominal_capacitance(args, cases)
    margins = print_margins(args, delays, c_nom)
    grid = print_power_grid(args, delays, power_grid)
    series = print_energy_balance(args, delays, c_nom)

    if not args.no_ascii:
        print("")
        print(ascii_plot(series, args.v_shutdown))
    if args.svg:
        svg_plot(series, args.v_shutdown, args.svg)
        print("\n  SVG curve: %s" % args.svg)

    report = build_report(args, delays, c_nom, reference, cases, margins, grid)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print("  JSON report: %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
