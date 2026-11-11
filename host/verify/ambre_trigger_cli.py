#!/usr/bin/env python3
"""Thin CLI: read a .vec file, emit the Python engine's .dec decisions on stdout.

Mirrors `ambre-dump-truth --vectors <in.vec>` so the two can be compared with:

    diff <(build/ambre-dump-truth --vectors v.vec) \
         <(python3 host/verify/ambre_trigger_cli.py --vectors v.vec)
"""

import argparse
import sys
from pathlib import Path

# Ensure the sibling module is importable regardless of CWD.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import ambre_trigger as at  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description="Emit Python .dec decisions for a .vec")
    parser.add_argument("--vectors", required=True, help="Input .vec path")
    args = parser.parse_args()

    scenarios = at.parse_vec(args.vectors)
    out = sys.stdout
    for line in at.dec_lines_for_scenarios(scenarios):
        out.write(line + "\n")
    out.flush()


if __name__ == "__main__":
    main()
