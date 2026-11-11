# Ambre scenarios

Synthetic drive scenarios for the Ambre EDR replay, exploration and trigger
tools. The scenario format is normative and described in
[`docs/FORMAT.md` §11](../../docs/FORMAT.md); this directory holds the
generator and a few reference files.

Everything here is **synthetic and privacy safe**: no VIN, no position, no
absolute date, no driver identity. A scenario only contains a name, a seed,
durations relative to the start of the drive, and sensor-like signals.

## Generate a scenario

```sh
python3 host/scenarios/generate.py \
    --preset urban-frontal --seed 1234 \
    --duration-ms 30000 --crash-at-ms 25000 \
    --out /tmp/urban-frontal.txt
```

| Option | Meaning |
|---|---|
| `--seed S` | Seed of the deterministic generator (`random.Random(S)`). Same seed, same bytes. |
| `--duration-ms N` | Length of the drive in milliseconds. The file has exactly `N` data lines. |
| `--crash-at-ms N` | Time of the impact. `0` means *no impact at all*. |
| `--out FILE` | Output path (required). |
| `--dv-kmh X` | Delta-v of the impact, in km/h (default `12`). |
| `--rate-hz 1000` | Sample rate. Only `1000` is supported: the format is one line per millisecond. |
| `--profile eu\|us` | Regulatory profile: `eu` (30 s, R160 class B) or `us` (20 s, 49 CFR 563). |
| `--preset NAME` | Use a named preset (see below). |
| `--list-presets` | Print the preset catalogue with one description line each. |

Python 3 standard library only, no network access.

## Format

```
# ambre scenario v1
name=urban-frontal
seed=1234
rate_hz=1000
duration_ms=30000
crash_at_ms=25000
# t_us a_long_mm_s2 a_lat_mm_s2 speed_kmh_x4 throttle brake rpm steer_x2 ext_flags
0 47 -6 102 2 0 1874 -1 0
1000 -4 -13 102 2 0 1874 -1 0
```

* The header is a set of `key=value` lines: `name`, `seed`, `rate_hz`,
  `duration_ms`, `crash_at_ms`.
* One comment line names the columns.
* Then **exactly one data line per millisecond**, so the number of data lines
  equals `duration_ms`.
* Nine columns separated by **one single space**, no trailing space, Unix
  `\n` line endings, no tabulation.

| Column | Unit | Note |
|---|---|---|
| `t_us` | µs | Monotonic, `i * 1000`. Never an absolute time. |
| `a_long_mm_s2` | mm/s² | Longitudinal acceleration, 1 kHz. |
| `a_lat_mm_s2` | mm/s² | Lateral acceleration, 1 kHz. |
| `speed_kmh_x4` | 0.25 km/h | Speed, integral of the longitudinal acceleration. |
| `throttle` | % | Accelerator pedal. |
| `brake` | 0/1 | Service brake. |
| `rpm` | rpm | Engine speed. |
| `steer_x2` | 0.5 deg | Steering wheel angle. |
| `ext_flags` | bitmask | bit0 restraint deployed, bit1 VRU, bit2 emergency brake, bit3 AEB, bit4 service command, bit5 sample invalid. |

The fast signals (accelerations) are sampled at 1 kHz. The slow signals
(pedals, engine speed, steering angle) are refreshed at 100 Hz and keep their
last value in between, as mandated by `docs/FORMAT.md` §11.

## Presets

| Preset | Description |
|---|---|
| `urban-frontal` | 30 s urban drive, 12 km/h frontal impact at 25 s, restraint fires. |
| `side-impact` | 20 s urban drive, 9 km/h lateral impact at 12 s, no restraint. |
| `rear-impact` | 20 s urban drive, 10 km/h rear impact (positive longitudinal) at 11 s. |
| `no-crash` | Clean 20 s urban drive, no impact at all. |
| `borderline-7.9` | 7.9 km/h in 150 ms: below DEC-01, must not trigger. |
| `borderline-8.1` | 8.1 km/h in 150 ms: above DEC-01, must trigger. |
| `borderline-151ms` | 8.1 km/h reached in 151 ms: ANCHORED mode must not trigger. |
| `multiple-events` | Two impacts 1.8 s apart: one accident, event 1 then event 2 (DEC-09). |
| `locked-airbag` | 30 km/h over 300 ms with restraint deployment: the event is locked (DEC-03). |

The three `borderline-*` presets use a *quiet* approach: the acceleration
alternates in equal-and-opposite pairs, so the integrated delta-v returns to
zero before the impact and the measured excursion is the impact itself. Their
post-impact residual braking is disabled, so nothing else pollutes the
measurement.

### Reference files

| File | How it was produced |
|---|---|
| `urban-frontal.txt` | `--preset urban-frontal --seed 1234 --duration-ms 30000 --crash-at-ms 25000` |
| `no-crash.txt` | `--preset no-crash --seed 7 --duration-ms 20000 --crash-at-ms 0` |
| `borderline-8.1.txt` | `--preset borderline-8.1 --seed 42` |

Regenerating a reference file with the same command must produce a
byte-identical file.

## Add a scenario

Two ways.

**1. From the command line**, without touching the code:

```sh
python3 host/scenarios/generate.py --duration-ms 20000 --crash-at-ms 15000 \
    --dv-kmh 9 --seed 99 --out /tmp/my-drive.txt
```

**2. As a named preset**, in `generate.py`:

1. Add an entry to the `PRESETS` dict:

   ```python
   "my-preset": {
       "description": "one line shown by --list-presets",
       "duration_ms": 20_000,
       "crashes": [dict(at=15_000, dv_kmh=9.0, pulse_ms=120, shape="crash",
                        sign=-1, restraint=False)],
   },
   ```

2. Impact options:

   | Key | Meaning |
   |---|---|
   | `at` | Impact time in ms. |
   | `dv_kmh` | Delta-v in km/h. |
   | `pulse_ms` | Duration of the impact pulse. |
   | `shape` | `crash` (trapezoid, realistic), `flat` (constant), `ramp` (rising), `late` (last-ms spike). |
   | `axis` | `long` (default) or `lat`. |
   | `sign` | `-1` decelerating (frontal), `+1` accelerating (rear). |
   | `restraint` | Set `True` to deploy the restraint (ext_flags bit0). |
   | `aeb` | Set `True` to flag an AEB intervention (ext_flags bit3). |
   | `pre_brake` | Set `True` to flag an emergency brake before the impact (ext_flags bit2). |

3. Optional preset keys: `quiet=True` for a delta-v neutral approach,
   `residual=<mm/s2>` for the post-impact deceleration.

The pulse is built so that the sum of its acceleration samples integrates to
`dv_kmh` exactly, and every impact sample is a multiple of 1000 mm/s², so
the integer rectangle rule of the trigger engine integrates the pulse without
any rounding loss.

## Plugging in real data later

The generator is the first of the three data sources listed in
`docs/cahier-des-charges.md` §8 ("Données du trajet"); the other two are
real drive recordings and real crash pulses. They are plugged in **behind the
same file format**, so no consumer changes.

### comma2k19

* Source: the comma2k19 driving dataset (MIT licence according to its card,
  to be confirmed).
* It provides CAN bus logs of real drives: wheel speeds, steering, pedal
  positions, engine speed.
* Conversion to an Ambre scenario:
  1. keep a single continuous drive segment (no ignition-off gap);
  2. resample the CAN signals to 1 kHz (linear interpolation of the 100 Hz
    wheel speeds), and compute the longitudinal and lateral accelerations by
     differentiating the wheel speeds;
  3. synthesise the vertical acceleration (suspension bounce) from the
     longitudinal acceleration, or leave it at zero if no source exists;
  4. write the header with `name=comma2k19-<drive-id>`, `seed=0`,
     `duration_ms=<number of milliseconds>`, `crash_at_ms=0`;
  5. **sanitise**: drop any field outside the signal whitelist
     (`docs/FORMAT.md` §5), drop GPS, VIN and timestamps, and shift the time
     base to zero.
* A converter would live in `host/scenarios/convert_comma2k19.py` and reuse
  the `render()` helper of `generate.py` so that only one writer exists.

### NHTSA crash data

* Source: the public NHTSA crash test database (format to be verified).
* It provides real impact pulses: deceleration traces of the vehicle body,
  sometimes with a vehicle-side and a barrier-side channel.
* Conversion to an Ambre scenario:
  1. take one impact trace, resample it to 1 kHz (the published traces are
     usually coarser);
  2. compute the delta-v of the trace and normalise it, then scale it to the
     `--dv-kmh` value to keep;
  3. place it at the requested `crash_at_ms`, with a plausible pre-crash
     approach and a post-impact residual deceleration;
  4. write the file with `render()`, `name=nhtsa-<test-id>`.
* The trace itself can be kept as a separate file in the repository and
  loaded by the converter, so the generated scenario stays reproducible.

Both converters keep the same invariants: no absolute time, no VIN, no
position, no driver identity, and one line per millisecond.
