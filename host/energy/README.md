# host/energy — hold-up energy model (spec 12.4)

Extreme-level item 12.4 of the cahier des charges: *"Energy reserve model. A
'voltage' input decays like a discharging capacitor. The demo shows that the
freeze ends before the shutdown threshold, and which capacitance is needed:
at 5 W, 20 ms require 0.1 J."*

`energy_model.py` is that model. Python 3, **standard library only**.

## Honesty statement (read first)

There is no QNX target yet, so **the freeze power and the freeze delay are
targets, not measurements** (the cahier itself lists "freeze delay of 20 ms"
as "target, to be measured", §14). Every number this tool prints for a delay
is labelled `target, not measured`, every parameter is overridable on the
command line, and all assumptions are published in the JSON report. The model
is a design tool — it sizes the hold-up capacitor and shows the margins — not
a measurement report.

## Model (RC, documented)

The recorder is fed by a hold-up capacitor `C`, charged at `V0`, through the
supply impedance `R` (wiring + source output impedance). At `t = 0` the supply
is cut; the capacitor alone feeds the recorder, which draws a **constant
power** `P` during the freeze:

```
I       = P / u                load current (constant-power load)
V_C     = u + I * R            capacitor voltage, u = load voltage
C dV_C/dt = -I                 capacitor discharge
=> du/dt = -P * u / (C * (u^2 - P*R))
```

integrated with RK4 at 1 µs. Two properties matter:

* the load voltage decays **faster and faster** (a constant-power load draws
  more current as the voltage sags);
* below `u = sqrt(P*R)` no steady state exists: the load **collapses**. The
  tool detects this and reports `COLLAPSED` instead of inventing a voltage.

Energy accounting is verified numerically at every run:
`E0 = E_load + E_R + E_end` (residual printed; ~1e-9 J with dt = 1 µs).

## The reference figure of the specification

The cahier quotes: *"at 5 W, 20 ms require 0.1 J"*. The tool recomputes it:

```
E = P x t = 5.0 W x 0.020 s = 0.100000 J      -> matches the cahier
```

and derives the capacitance that actually delivers it from `V0` down to the
shutdown threshold `Vsd`, with and without the series loss:

```
energy-only (R = 0):  C = 2 P t / (V0^2 - Vsd^2)
full RC model:        smallest C with u(t_freeze) >= Vsd   (bisection)
```

## Usage

```sh
python3 host/energy/energy_model.py                       # defaults below
python3 host/energy/energy_model.py --power 5 --delays 5,10,20,50 \
    --json build/energy.json --svg build/energy.svg
```

| option | default | meaning |
|---|---|---|
| `--v0` | 12.6 V | load voltage at the instant of the cut |
| `--v-shutdown` | 9.0 V | recorder shutdown threshold |
| `--r` | 0.2 Ohm | series resistance of the supply path |
| `--power` | 5 W | freeze power (target, not measured) |
| `--delays` | 5,10,20,50 | freeze delays in ms (targets, not measured) |
| `--c-nom` | 2 x C_min(longest delay) | nominal capacitance for the margin table |
| `--json` | none | JSON report (assumptions + every table) |
| `--svg` | none | standalone SVG of the voltage curves (hand-built, no library) |
| `--no-ascii` | off | skip the ASCII curve on stdout |

Exit code is 0 unless the tool itself fails.

## Output

1. the **reference check** (0.1 J reproduced from P x t);
2. a **per-delay table**: energy needed, minimum capacitance (RC model and
   energy-only), end-of-freeze voltage — every row marked
   `target, not measured`;
3. a **margin table** at the nominal capacitance: end voltage, voltage margin
   (V and %), usable energy, collapse state;
4. a **power x delay grid** of minimum capacitances;
5. the **voltage curves** for 5/10/20/50 ms as an ASCII plot on stdout and as
   a standalone SVG (inline styles, zero external resource);
6. a **JSON report** with the parameters, the model equations, the collapse
   threshold, every table and the honesty labels.

## Measured run (development PC, defaults)

`python3 host/energy/energy_model.py --json build-qual2/energy.json --svg build-qual2/energy.svg`

| delay (target) | energy needed | C_min (RC) | C_min (energy-only) |
|---|---|---|---|
| 5 ms | 0.0250 J | 0.649 mF | 0.643 mF |
| 10 ms | 0.0500 J | 1.297 mF | 1.286 mF |
| 20 ms | **0.100000 J** (the cahier's 0.1 J) | **2.594 mF** | 2.572 mF |
| 50 ms | 0.2500 J | 6.486 mF | 6.430 mF |

At the nominal C = 12.972 mF (twice the 50 ms minimum) every delay ends above
the 9.0 V threshold with >= 21.7 % voltage margin; energy-balance residual
<= 1.1e-8 J.

## Limits

* single capacitor, single constant-power load: no DC/DC converter behaviour,
  no battery re-entry, no temperature derating of the capacitance;
* `R` lumps wiring, source impedance and capacitor ESR into one series value;
* the collapse threshold `sqrt(P*R)` is a property of the constant-power
  model, not of the real hardware;
* the freeze power/delay remain **targets** until a QNX target exists — rerun
  this tool with the measured values (`--power`, `--delays`) and the JSON
  `measured` field flipped in the code constant `MEASURED`.
