# Data sources for the demonstration

The demonstration drive comes from three sources, in this fallback order (cahier
des charges §8). All three end up in **one format**: the scenario file of
[`docs/FORMAT.md` §11](../../docs/FORMAT.md).

| Priority | Source | Tool | Always available |
|---|---|---|---|
| 1 | Synthetic generator, fixed seed | `host/data/synthetic.py` (thin layer over `host/scenarios/generate.py`) | yes |
| 2 | Real driving recordings, e.g. comma2k19 | `host/data/import_comma2k19.py` | no (see the licence note) |
| 3 | Real crash pulses from the public NHTSA crash-test database | `host/data/import_nhtsa.py` | no (see the format note) |

## Prudence notes carried over from the cahier des charges

Two claims in the cahier are explicitly **not verified**, and this directory
repeats them rather than quietly upgrading them (§14, "Ce qui est vérifié et ce
qui reste à valider"):

> **comma2k19 license: MIT according to its own page, to be confirmed.**
> The cahier cites the dataset as "licence MIT d'après sa fiche, à confirmer".
> Nothing here re-licenses it, and no comma2k19 data is vendored: the fixtures
> are synthetic and carry a "NOT real comma2k19 data" banner.

> **NHTSA data format: to be verified.**
> The cahier writes "base publique des essais de choc de la NHTSA (format à
> vérifier)". The importer therefore describes the *channels* it needs, not a
> file layout it cannot confirm; see the correspondence table below.

A third caveat matters for the demonstration: **no real data is shipped in this
repository**. Everything under `fixtures/` is synthetic, generated for these
tests, with no VIN, no position and no absolute date. A run with the real
datasets requires the user to fetch them (offline, under their own licence).

## What each importer consumes

Neither importer depends on `h5py`, `numpy` or the network: both read a
**flattened CSV extract** of one source record. Swapping in a genuine reader
means replacing exactly one function (`read_extract` / `read_pulse`); the rest
of the pipeline — units, privacy filtering, resampling, serialization — stays
unchanged and keeps its tests.

### comma2k19 (`import_comma2k19.py`)

The published dataset is one HDF5 file per ~1-minute segment. The extract used
here is the segment's series aligned on one 100 Hz tick grid:

| Real series | Extract column | Kept? | Becomes |
|---|---|---|---|
| `processed_log/IMU/<n>/accel_x` | `accel_x` (m/s²) | yes | `a_long_mm_s2` |
| `processed_log/IMU/<n>/accel_y` | `accel_y` (m/s²) | yes | `a_lat_mm_s2` |
| `processed_log/IMU/<n>/accel_z`, `gyro_x`, `gyro_y` | same names | dropped | — (not class A fields) |
| `processed_log/CAN/<n>/speed` | `speed` (m/s) | yes | `speed_kmh_x4` |
| `pose[3]` of the route | `pos_x`, `pos_y`, `pos_z` | **dropped** | — (position, VIE-01) |
| `processed_log/GPS/<n>/lat,lon` | `lat`, `lon` | **dropped** | — (position, VIE-01) |
| — | `wheel_speed_mps` | check only | — |

Privacy is decided at import, in `to_sample()` and in the column allow-list:
position, GNSS and every absolute time are dropped **before** anything is
written, so they cannot reach a scenario file. Only `t_ms` relative to the
segment start is used, and it is rebased to zero.

The extract has no engine speed, pedal or steering channel. Those class A
fields therefore stay at zero: the importer does not invent them.

### NHTSA crash tests (`import_nhtsa.py`)

The extract is one test's vehicle accelerations, decimated from the 10 kHz raw
channels to the 1 ms grid:

| Channel | Extract column | Becomes |
|---|---|---|
| "Vehicle Longitudinal Acceleration" (SAE J211, in g) | `accel_long_g` | `a_long_mm_s2` (× 9806.65) |
| "Vehicle Lateral Acceleration" (in g) | `accel_lat_g` | `a_lat_mm_s2` |
| "Vehicle Velocity Change" (delta-v, km/h) | `velocity_kmh` | **check only**, never written |

Why the delta-v column is not written into the scenario: a scenario carries
accelerations and the trigger engine integrates them itself (contract A3 of
`fmt/ambre_trigger.h`). Writing the recorded delta-v as well would give two
sources for one quantity. Instead the importer **integrates the pulse with the
engine's own rectangle rule and fails** when the result disagrees with the
recorded column by more than 2 % — that check is what proves the `g` → `mm/s²`
conversion, since a unit mix-up is off by 3.6×.

## Synthetic (`synthetic.py`)

`host/data/synthetic.py` is a **thin layer**, not a second generator: it imports
`host/scenarios/generate.py` by path and reuses `pulse_steps()`, `pulse shapes`,
`build_config()`, `simulate()` and `render()`. It adds only what the importers
need:

* `Sample`, the in-memory row model;
* `resample()`, the zero-order hold into the 1 ms grid (no interpolation, no
  invented value);
* `inject_pulse()`, which *replaces* the recorded acceleration with a pulse
  instead of adding to it — adding two measurements would invent a third;
* `write_scenario()`, which calls `generate.render()` so the header, the column
  comment and the spacing stay byte-identical to the generator's output.

## Usage

```sh
# 1. synthetic (always works)
python3 host/data/synthetic.py --preset urban-frontal --seed 1234 \
    --duration-ms 30000 --crash-at-ms 25000 --out /tmp/synthetic.txt

# 2. comma2k19 segment extract, with a splice-in pulse
python3 host/data/import_comma2k19.py \
    --fixture host/data/fixtures/comma2k19_segment_sample.csv \
    --out /tmp/comma2k19.txt --crash-at-ms 1500

# 3. NHTSA crash pulse
python3 host/data/import_nhtsa.py \
    --fixture host/data/fixtures/nhtsa_pulse_sample.csv \
    --out /tmp/nhtsa.txt

# every output goes through the same consumers
build/ambre-explore --scenario /tmp/nhtsa.txt --states 100
python3 -c 'import sys; ...'   # or the trigger twin, see tests/test_data_py.py
```

All three write the same nine columns, one line per millisecond, and nothing
else. `python3 tests/test_data_py.py` checks that, in both importers.

## What the tests prove

`tests/test_data_py.py` asserts, for each importer:

1. the output is a valid §11 scenario: `key=value` header with the five keys,
   exactly nine columns per data line, one line per millisecond, no trailing
   space, no tabulation;
2. **no forbidden data (VIE-01)**: no VIN, no position, no absolute time — the
   column set is checked, and every `t_us` is a small multiple of 1000 (a
   relative grid, not an epoch);
3. **it triggers**: the imported scenario is fed to the Python trigger twin
   `host/verify/ambre_trigger.py` (imported, never copied), which must fire at
   least one event at the expected instant.

## Files

| File | Role |
|---|---|
| `synthetic.py` | thin layer over `host/scenarios/generate.py` + CLI |
| `import_comma2k19.py` | comma2k19 segment extract → scenario |
| `import_nhtsa.py` | NHTSA crash pulse → scenario |
| `fixtures/comma2k19_segment_sample.csv` | synthetic extract, mapped like a real segment |
| `fixtures/nhtsa_pulse_sample.csv` | synthetic pulse, self-consistent delta-v |
| `README.md` | this document |

Python 3 standard library only, no network access.
