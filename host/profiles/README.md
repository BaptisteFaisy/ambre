# Regulatory profiles

Two selectable profiles for the Ambre EDR, and one measured limit.

| Profile | Window before T0 | Rate | Points | Source |
|---|---|---|---|---|
| `eu` | 5 s | 2 Hz | 10 | UN R160, cahier des charges §4.1 |
| `us` | 20 s | 10 Hz | 200 | 49 CFR Part 563, cahier §2 and §6.2 |

The American profile is **derived from class B by subsampling**, never by a
second acquisition path: class B already carries the accelerations at 1 kHz,
the dynamics at 100 Hz and the states at 10 Hz (cahier §6.2), so 10 Hz is a
decimation of what is already written.

```python
from host.profiles import us_profile, subsampling_plan, verdict

print(subsampling_plan(us_profile()))
# {'accel': 100, 'dynamics': 10, 'states': 1}

print(verdict()["frozen_layout_bytes"])   # 4032
```

`python3 host/profiles/us_profile.py` prints the full comparison.

## Measured size of a US `EMERG` record

Two honest encodings are reported, because the cahier and the frozen struct do
not express the same thing.

| Encoding | Per point | Points | Payload | Blocks (460 B) | One 512 B block? | One 16-block record? |
|---|---|---|---|---|---|---|
| Frozen struct layout (`ambre_emerg_t`: 32 B header + `points[]`) | 20 B | 200 | **4 032 B** | 9 | no | yes |
| Generic sizing of the cahier (§4.1, "~16 Ko for 20 signals") | 20 signals × 4 B | 200 | **16 020 B** | 35 | no | no |

Where the numbers come from, with no invented digit:

* `ambre_a_point_t` is 20 bytes (ten regulatory fields, `docs/FORMAT.md` §6) and
  `ambre_emerg_t` is a 32-byte header plus `points[21]`, so its layout at 200
  points is `32 + 200 × 20 = 4 032` bytes.
* The generic model is the one the cahier uses for its own sizing: one `int32`
  per data element (20 elements, cahier §6.1) per snapshot, i.e.
  `20 × 4 × 200 = 16 000` bytes of samples plus a 20-byte header descriptor.

### Verdict

**A US `EMERG` never fits in one 512-byte block, in either encoding.** The
usable payload of a block is 460 bytes (`512 − 48 − 4`, `docs/FORMAT.md` §2),
which is under a single snapshot of 20 signals (80 bytes) times even three
points.

**Multi-block is mandatory**, and the two encodings do not have the same
headroom:

* the **frozen struct layout** (4 032 B → 9 blocks) still fits one record: the
  largest legal record is `16 × 460 = 7 360` bytes (`AMBRE_REC_MAX_BLOCKS`);
* the **generic cahier sizing** (16 020 B → 35 blocks) exceeds it by more than
  twice, so a US event would have to be split over **3 records**.

## The frozen-struct limit

`fmt/ambre_types.h` is **frozen** for this task and is not modified:
`ambre_emerg_t.points[21]` holds 21 snapshots, which is 10 points at 2 Hz with
a margin for the European profile (5 s at 2 Hz), but only **2.1 s at 10 Hz** —
one tenth of the American window. The struct therefore cannot carry a US
pre-crash snapshot as it stands.

A production change would extend the class A structure (or add a US variant
with `points[200]`, 4 032 bytes of payload) and let the EMERG record span
several blocks. That is a format change and belongs to a version bump, not to
this task, so the limit is documented here and measured by a test instead.

## What is proved, and where

| Claim | Proof |
|---|---|
| 20 s at 10 Hz is 200 points, derived from class B | `subsampling_plan()`, `tests/test_profile_us_py.py` |
| Frozen layout: 4 032 B, 9 blocks | `test_us.c` `us_profile_record_size`, `test_profile_us_py.py` |
| Generic sizing: 16 020 B, 35 blocks | same, both languages |
| Neither fits one block | same, both languages |
| 9 blocks fit one record, 35 do not | same, both languages |
| The multi-block mechanism carries 4 032 B and 7 360 B intact | `test_us.c` `us_profile_multiblock` |
| A 35-block record is refused, not truncated | `test_us.c`, `ambre_block_build_chunk` returns `AMBRE_ERR_TOO_BIG` |
| The frozen `ambre_emerg_t` covers 2.1 s at 10 Hz | `points_in_emerg()`, `window_covered_ms()` |

C proof: `build/test_policy_us`. Python proof: `python3 tests/test_profile_us_py.py`.

## Files

| File | Role |
|---|---|
| `us_profile.py` | profile definitions, subsampling plan, size model, verdict, report |
| `README.md` | this document |

No network access, Python 3 standard library only.
