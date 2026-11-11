#!/usr/bin/env python3
"""Shared binary-discovery helper for the live-service Python suites.

Kept out of the ``test_*_py.py`` glob on purpose: this module holds no test,
only the one place where the PC suites locate the built process binaries
(same convention as ``tests/test_e2e_py.py``: ``AMBRE_BINS`` /
``AMBRE_BUILD_DIR``, then ``build/``, then ``build-e2e/``). A skipped lookup
would look like a green test, so a missing build fails loudly.
"""

from __future__ import annotations

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_bins(required=("edr-journal", "edrctl")) -> str:
    """Return the build directory holding the Ambre process binaries."""
    candidates = []
    for var in ("AMBRE_BINS", "AMBRE_BUILD_DIR"):
        value = os.environ.get(var)
        if value:
            candidates.append(value if os.path.isabs(value)
                              else os.path.join(ROOT, value))
    candidates += [os.path.join(ROOT, "build"), os.path.join(ROOT, "build-e2e")]
    for path in candidates:
        if all(os.access(os.path.join(path, name), os.X_OK)
               for name in required):
            return path
    raise RuntimeError(
        "no built binaries %s (looked in %s): run 'make all' first"
        % (list(required), ", ".join(candidates)))
