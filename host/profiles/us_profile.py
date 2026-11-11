"""US EDR profile (49 CFR Part 563) and its record size, in Python.

The cahier des charges describes two regulatory profiles:

* the European one (UN R160): class A keeps ``5 s before T0 at 2 Hz``, which is
  10 points (cahier section 4.1, table row "Fenêtre");
* the American one (49 CFR Part 563, final rule of 18 December 2024): the
  pre-crash window becomes ``20 s at 10 Hz`` instead of ``5 s at 2 Hz``
  (cahier section 2, line "49 CFR Part 563"), i.e. 200 points.

The American class A is *derived from class B by subsampling* (cahier
section 6.2, last line): class B already carries the accelerations at 1 kHz,
the dynamics at 100 Hz and the states at 10 Hz (cahier section 6.2 table), so
10 Hz is obtained by decimation, never by a second acquisition path.

This module answers one question with measured numbers and no estimate: what
does a US ``EMERG`` payload weigh, and does it fit in the journal format of
``docs/FORMAT.md`` (512-byte blocks, 460 usable payload bytes each, a record
spanning at most 16 blocks)?

Every constant below is either a frozen value of the format (read from
``fmt/ambre_types.h``) or a figure the cahier des charges states; none is
invented. The size model is explicit so it can be checked line by line.

Run the proof::

    python3 tests/test_profile_us_py.py
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List

# --- frozen format constants (fmt/ambre_types.h, docs/FORMAT.md sections 2, 3)
BLOCK_SIZE = 512                 # one sector, AMBRE_BLOCK_SIZE
BLOCK_PAYLOAD_MAX = 460          # AMBRE_BLOCK_PAYLOAD_MAX = 512 - 48 - 4
REC_MAX_BLOCKS = 16              # AMBRE_REC_MAX_BLOCKS: one record <= 16 blocks
BLOCK_HEADER_BYTES = 48          # AMBRE_BLOCK_HDR_SIZE
BLOCK_CRC_BYTES = 4              # whole-block CRC-32C

# --- frozen class A structures (fmt/ambre_types.h)
CLASS_A_POINT_BYTES = 20         # ambre_a_point_t
EMERG_FIXED_HEADER_BYTES = 32    # ambre_emerg_t without its points[] array
EMERG_POINTS = 21                # ambre_emerg_t.points[21]

# --- profiles (cahier des charges sections 2, 4.1, 6.1, 6.2)
EU_WINDOW_MS = 5_000             # "5 s avant T0"
EU_RATE_HZ = 2                   # "2 points/s"
US_WINDOW_MS = 20_000            # "20 s à 10 Hz"
US_RATE_HZ = 10
# "environ 16 Ko pour 20 signaux" (section 4.1): the sizing model is one int32
# per data element per snapshot. The 20 data elements are the ones listed in
# section 6.1 (the regulatory class A set).
US_SIGNALS = 20
US_VALUE_BYTES = 4               # int32, one value per signal per snapshot
US_HEADER_BYTES = 20             # event id, flags, timestamps, point count
US_POINTS = US_WINDOW_MS * US_RATE_HZ // 1000   # 200 snapshots (20 s at 10 Hz)

# Class B capture rates (cahier section 6.2 table, docs/FORMAT.md section 5).
CLASS_B_GROUPS = (
    ("accel", 1_000),            # x, y, z accelerations
    ("dynamics", 100),           # wheel speeds, steering, pedals, yaw, rpm
    ("states", 10),              # ADAS, belts, lamps, gear
)


@dataclass(frozen=True)
class Profile:
    """One regulatory profile: window, rate, snapshot width and budget."""

    name: str
    window_ms: int
    rate_hz: int
    signals: int
    value_bytes: int
    header_bytes: int

    @property
    def points(self) -> int:
        """Number of class A snapshots in the pre-crash window."""
        return self.window_ms * self.rate_hz // 1000

    @property
    def period_ms(self) -> int:
        """Interval between two snapshots, in milliseconds."""
        return 1000 // self.rate_hz

    @property
    def point_bytes(self) -> int:
        """Size of one snapshot, all signals included."""
        return self.signals * self.value_bytes

    @property
    def payload_bytes(self) -> int:
        """Size of the whole class A payload: header plus every snapshot."""
        return self.header_bytes + self.points * self.point_bytes


def eu_profile() -> Profile:
    """European profile: 5 s at 2 Hz, 10 snapshots (cahier section 4.1)."""
    return Profile("eu", EU_WINDOW_MS, EU_RATE_HZ, US_SIGNALS, US_VALUE_BYTES,
                   US_HEADER_BYTES)


def us_profile() -> Profile:
    """US profile: 20 s at 10 Hz, 200 snapshots (cahier section 2)."""
    return Profile("us", US_WINDOW_MS, US_RATE_HZ, US_SIGNALS, US_VALUE_BYTES,
                   US_HEADER_BYTES)


def compact_us_profile() -> Profile:
    """US profile stored with the frozen 20-byte class A point.

    ``ambre_a_point_t`` already packs the regulatory fields in 20 bytes, so a
    snapshot costs 20 bytes instead of 20 signals x 4 bytes. This is the
    smallest honest encoding of the same window; it is reported next to the
    generic model so the reader sees both bounds.
    """
    return Profile("us-compact", US_WINDOW_MS, US_RATE_HZ, 1,
                   CLASS_A_POINT_BYTES, US_HEADER_BYTES)


def decimation_factor(class_b_rate_hz: int, profile_rate_hz: int) -> int:
    """How many class B samples are dropped per kept sample (section 6.2)."""
    if profile_rate_hz <= 0:
        raise ValueError("profile_rate_hz must be positive")
    if class_b_rate_hz % profile_rate_hz != 0:
        raise ValueError("%d Hz is not a multiple of %d Hz"
                         % (class_b_rate_hz, profile_rate_hz))
    return class_b_rate_hz // profile_rate_hz


def subsampling_plan(profile: Profile) -> Dict[str, int]:
    """Decimation factor to apply to each class B group (cahier 6.2)."""
    return {group: decimation_factor(rate, profile.rate_hz)
            for group, rate in CLASS_B_GROUPS}


def blocks_needed(payload_bytes: int,
                  block_payload: int = BLOCK_PAYLOAD_MAX) -> int:
    """Blocks a record occupies, rounded up (docs/FORMAT.md section 2)."""
    if payload_bytes <= 0:
        return 1
    return (payload_bytes + block_payload - 1) // block_payload


def largest_record_payload() -> int:
    """Bytes the biggest legal record can carry: 16 x 460 = 7 360."""
    return REC_MAX_BLOCKS * BLOCK_PAYLOAD_MAX


def fits_one_block(payload_bytes: int) -> bool:
    """True when the payload fits a single 512-byte block."""
    return payload_bytes <= BLOCK_PAYLOAD_MAX


def fits_one_record(payload_bytes: int) -> bool:
    """True when the payload fits the largest legal record (16 blocks)."""
    return payload_bytes <= largest_record_payload()


def emerg_payload_capacity() -> int:
    """Bytes of points the frozen ``ambre_emerg_t`` can carry."""
    return EMERG_POINTS * CLASS_A_POINT_BYTES


def frozen_us_emerg_bytes(points: int = 0) -> int:
    """Real size of a US EMERG stored with the frozen struct layout.

    ``ambre_emerg_t`` is a 32-byte fixed header followed by ``points[21]`` of
    20 bytes each. Storing 200 points in that layout therefore costs
    32 + 200 x 20 = 4 032 bytes, whatever the number of signals: the 20-byte
    ``ambre_a_point_t`` already packs every regulatory field.
    """
    count = points if points > 0 else us_profile().points
    return EMERG_FIXED_HEADER_BYTES + count * CLASS_A_POINT_BYTES


def verdict() -> Dict[str, object]:
    """Does a US EMERG fit one block, one record, or neither?"""
    generic = us_profile().payload_bytes
    frozen = frozen_us_emerg_bytes()
    return {
        "block_payload_max": BLOCK_PAYLOAD_MAX,
        "rec_max_blocks": REC_MAX_BLOCKS,
        "largest_record_payload": largest_record_payload(),
        "generic_model_bytes": generic,
        "frozen_layout_bytes": frozen,
        "generic_fits_block": fits_one_block(generic),
        "frozen_fits_block": fits_one_block(frozen),
        "generic_fits_record": fits_one_record(generic),
        "frozen_fits_record": fits_one_record(frozen),
        "generic_blocks": blocks_needed(generic),
        "frozen_blocks": blocks_needed(frozen),
        "generic_records": records_needed(generic),
        "frozen_records": records_needed(frozen),
        "emerg_points": points_in_emerg(),
        "emerg_window_ms": window_covered_ms(points_in_emerg(), US_RATE_HZ),
    }


def points_in_emerg() -> int:
    """Snapshots the frozen ``ambre_emerg_t`` holds (cahier 6.1 margin)."""
    return EMERG_POINTS


def window_covered_ms(points: int, rate_hz: int) -> int:
    """Milliseconds of pre-crash history a point count covers."""
    if rate_hz <= 0:
        raise ValueError("rate_hz must be positive")
    return points * 1000 // rate_hz


def records_needed(payload_bytes: int) -> int:
    """Records needed when one record cannot carry the whole payload."""
    if fits_one_record(payload_bytes):
        return 1
    capacity = largest_record_payload()
    return (payload_bytes + capacity - 1) // capacity


def describe(profile: Profile) -> Dict[str, object]:
    """Every measured value for one profile, ready to print."""
    blocks = blocks_needed(profile.payload_bytes)
    return {
        "profile": profile.name,
        "window_ms": profile.window_ms,
        "rate_hz": profile.rate_hz,
        "period_ms": profile.period_ms,
        "points": profile.points,
        "signals": profile.signals,
        "point_bytes": profile.point_bytes,
        "payload_bytes": profile.payload_bytes,
        "blocks_needed": blocks,
        "single_block": fits_one_block(profile.payload_bytes),
        "single_record": fits_one_record(profile.payload_bytes),
        "records_needed": records_needed(profile.payload_bytes),
        "largest_record_payload": largest_record_payload(),
    }


def report() -> str:
    """Human-readable comparison of the two profiles and the frozen limit."""
    rows: List[Dict[str, object]] = [describe(eu_profile()),
                                     describe(us_profile()),
                                     describe(compact_us_profile())]
    lines = ["%-18s %8s %8s %8s %9s %8s %7s %7s"
             % ("profile", "window", "rate", "points", "payload", "blocks",
                "1 block", "1 record")]
    for r in rows:
        lines.append("%-18s %8d %8d %8d %9d %8d %7s %8s"
                     % (r["profile"], r["window_ms"], r["rate_hz"], r["points"],
                        r["payload_bytes"], r["blocks_needed"],
                        yes_no(r["single_block"]), yes_no(r["single_record"])))
    us = describe(us_profile())
    lines.append("")
    lines.append("US payload: %d bytes -> %d blocks of %d B (max %d per record)"
                 % (us["payload_bytes"], us["blocks_needed"], BLOCK_PAYLOAD_MAX,
                    REC_MAX_BLOCKS))
    lines.append("largest legal record: %d bytes; US payload needs %d record(s)"
                 % (largest_record_payload(), us["records_needed"]))
    lines.append("frozen ambre_emerg_t: %d points (%d ms at %d Hz), %d B of points"
                 % (points_in_emerg(), window_covered_ms(points_in_emerg(),
                                                         US_RATE_HZ),
                    US_RATE_HZ, emerg_payload_capacity()))
    lines.append("subsampling plan from class B: %s"
                 % subsampling_plan(us_profile()))
    v = verdict()
    lines.append("")
    lines.append("VERDICT: a US EMERG never fits one 512 B block "
                 "(block payload = %d B)." % v["block_payload_max"])
    lines.append("  generic model (20 signals x 4 B): %d B -> %d blocks, "
                 "%d record(s)" % (v["generic_model_bytes"], v["generic_blocks"],
                                   v["generic_records"]))
    lines.append("  frozen struct layout (20 B/point): %d B -> %d blocks, "
                 "%d record(s)" % (v["frozen_layout_bytes"], v["frozen_blocks"],
                                   v["frozen_records"]))
    lines.append("  multi-block is mandatory in both cases; the frozen "
                 "ambre_emerg_t covers only %d ms at %d Hz"
                 % (v["emerg_window_ms"], US_RATE_HZ))
    return "\n".join(lines)


def yes_no(value: object) -> str:
    """Render a boolean as yes/no (the report is read by humans)."""
    return "yes" if value else "no"


if __name__ == "__main__":
    print(report())
