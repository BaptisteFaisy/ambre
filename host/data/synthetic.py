#!/usr/bin/env python3
"""Synthetic drive scenarios — the always-available data source (cahier §8).

This module is a **thin layer over ``host/scenarios/generate.py``**, which it
imports instead of re-implementing: the drive model, the impact pulse shapes
and the scenario serializer all come from there (house rule: one
implementation per logic). What this module adds, and nothing else:

* a small in-memory row model (``Sample``) so an importer can post-process a
  real recording and still write a scenario file;
* **resampling** (zero-order hold) to the 1 kHz grid the format mandates;
* **pulse injection**, reusing ``generate.pulse_steps()`` so a recorded drive
  with no collision in it can be spliced with a real-test crash pulse;
* ``write_scenario()``, which calls ``generate.render()``.

Command line: a pass-through to the generator, so the demo has one entry point
per source (synthetic, comma2k19, NHTSA) with the same options.

    python3 host/data/synthetic.py --preset urban-frontal --seed 1234 \\
        --duration-ms 30000 --crash-at-ms 25000 --out /tmp/scenario.txt

No network access, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Sequence

DATA_DIR = Path(__file__).resolve().parent
REPO_ROOT = DATA_DIR.parent.parent
GENERATOR_PATH = REPO_ROOT / "host" / "scenarios" / "generate.py"

# Scenario grid (docs/FORMAT.md §11): one line per millisecond.
GRID_MS = 1
GRID_US = GRID_MS * 1000

# ext_flags bits (docs/FORMAT.md §11).
EXT_RESTRAINT = 1 << 0
EXT_VRU = 1 << 1
EXT_EMERGENCY_BRAKE = 1 << 2
EXT_AEB = 1 << 3
EXT_SERVICE = 1 << 4
EXT_INVALID = 1 << 5

COLUMNS = ("t_us", "a_long_mm_s2", "a_lat_mm_s2", "speed_kmh_x4", "throttle",
           "brake", "rpm", "steer_x2", "ext_flags")
FIELD_COUNT = len(COLUMNS)


def generator():
    """Import ``host/scenarios/generate.py`` by path, bound to no package."""

    spec = importlib.util.spec_from_file_location("ambre_generate",
                                                  GENERATOR_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load %s" % GENERATOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@dataclass
class Sample:
    """One scenario line, in the units of docs/FORMAT.md §11."""

    a_long_mm_s2: int = 0
    a_lat_mm_s2: int = 0
    speed_kmh_x4: int = 0
    throttle: int = 0
    brake: int = 0
    rpm: int = 0
    steer_x2: int = 0
    ext_flags: int = 0

    def as_row(self) -> tuple:
        """The eight data columns, in the normative order."""
        return (self.a_long_mm_s2, self.a_lat_mm_s2, self.speed_kmh_x4,
                self.throttle, self.brake, self.rpm, self.steer_x2,
                self.ext_flags)


def frames(duration_ms: int) -> List[Sample]:
    """An empty drive: one sample per millisecond, all channels at rest."""
    return [Sample() for _ in range(duration_ms)]


def resample(samples: Sequence[Sample], source_period_ms: int) -> List[Sample]:
    """Zero-order hold into the 1 kHz grid.

    A source sampled at ``source_period_ms`` is expanded by repeating each
    sample, which is exactly what a hold does and what the format expects
    ("slow signals keep their last value", docs/FORMAT.md §11). The last
    sample is held up to the next one, so no value is ever interpolated or
    invented.
    """
    if source_period_ms <= 0:
        raise ValueError("source_period_ms must be positive")
    factor = source_period_ms // GRID_MS
    if factor < 1:
        raise ValueError("source period %d ms is finer than the 1 ms grid"
                         % source_period_ms)
    out: List[Sample] = []
    for sample in samples:
        out.extend(Sample(**vars(sample)) for _ in range(factor))
    return out


def pulse_samples(dv_kmh: float, pulse_ms: int, shape: str,
                  sign: int = -1, axis: str = "long") -> List[Sample]:
    """One impact as samples, using the generator's own pulse integrator."""
    steps = generator().pulse_steps(dv_kmh, pulse_ms, shape)
    out: List[Sample] = []
    for step in steps:
        accel = sign * step * 1000          # mm/s per ms -> mm/s^2
        if axis == "lat":
            out.append(Sample(a_lat_mm_s2=accel))
        else:
            out.append(Sample(a_long_mm_s2=accel, brake=1))
    return out


def inject_pulse(samples: List[Sample], pulse: Sequence[Sample],
                 at_ms: int) -> List[Sample]:
    """Overwrite ``samples[at_ms:]`` with an impact pulse, clipped to the end.

    The pulse replaces the recorded acceleration instead of being added to it:
    a crash-test channel is a measurement, not an increment, and adding the two
    would invent values that neither source contains.
    """
    if at_ms < 0:
        raise ValueError("at_ms must be non-negative")
    for offset, patch in enumerate(pulse):
        index = at_ms + offset
        if index >= len(samples):
            break
        target = samples[index]
        target.a_long_mm_s2 = patch.a_long_mm_s2
        target.a_lat_mm_s2 = patch.a_lat_mm_s2
        target.brake = patch.brake
        target.ext_flags |= patch.ext_flags
    return samples


def flag_span(samples: List[Sample], start_ms: int, end_ms: int,
              bit: int) -> None:
    """OR one ext_flags bit over a window, clipped to the drive (in place)."""
    for index in range(max(0, start_ms), min(end_ms, len(samples))):
        samples[index].ext_flags |= bit


def write_scenario(path: str, name: str, seed: int, samples: Sequence[Sample],
                   crash_at_ms: int, rate_hz: int = 1000) -> int:
    """Serialize with the generator's renderer; returns the line count.

    Reusing ``generate.render()`` is deliberate: the header keys, the column
    comment and the single-space formatting must stay byte-identical to what
    ``host/scenarios/generate.py`` produces, otherwise the exploration and the
    verifier would see two dialects of the same format.
    """
    gen = generator()
    cfg = {
        "name": name,
        "seed": seed,
        "rate_hz": rate_hz,
        "duration_ms": len(samples),
        "crash_at_ms": crash_at_ms,
    }
    rows = [sample.as_row() for sample in samples]
    text = gen.render(cfg, rows)
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        handle.write(text)
    return len(rows)


def synthetic_drive(preset: str, seed: int, duration_ms: int,
                    crash_at_ms: int) -> List[Sample]:
    """Run the generator's model and return its rows as samples.

    ``--crash-at-ms 0`` disables the impact, exactly like the generator.
    """
    gen = generator()
    argv = ["--seed", str(seed), "--duration-ms", str(duration_ms),
            "--crash-at-ms", str(crash_at_ms), "--preset", preset,
            "--out", "/dev/null"]
    cfg = gen.build_config(gen.parse_args(argv))
    import random
    rng = random.Random(seed)
    cfg["start_speed_mm_s"] = float(rng.randint(7000, 14_000))
    rows = gen.simulate(cfg, rng)
    return [Sample(*row) for row in rows]


def count_lines(path: str) -> int:
    """Number of data lines in a scenario file (for the tests and the CLI)."""
    total = 0
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if "=" in stripped.split()[0]:
                continue
            total += 1
    return total


def parse_args(argv: Iterable[str]):
    """Pass-through to the generator's own options, plus the output path."""
    parser = argparse.ArgumentParser(
        description="Synthetic Ambre scenario (thin wrapper over "
                    "host/scenarios/generate.py)")
    parser.add_argument("--preset", default="urban-frontal")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--duration-ms", type=int, default=30000)
    parser.add_argument("--crash-at-ms", type=int, default=25000,
                        help="0 disables the impact")
    parser.add_argument("--out", required=True)
    return parser.parse_args(list(argv))


def main(argv=None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    samples = synthetic_drive(args.preset, args.seed, args.duration_ms,
                              args.crash_at_ms)
    written = write_scenario(args.out, args.preset, args.seed, samples,
                             args.crash_at_ms)
    print("%s: %d samples, seed %d, preset %s"
          % (args.out, written, args.seed, args.preset))
    return 0


if __name__ == "__main__":
    sys.exit(main())
