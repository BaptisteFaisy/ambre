# Ambre journal format — v1 (normative)

This file is the **contract** between the C library (`fmt/`), the QNX targets
(`target/`) and the PC tools (`host/`). `fmt/ambre_types.h` is the machine
readable version of the same contract; if the two disagree, the header wins
and this file must be fixed.

Everything is **little-endian**, packed (no padding), and only ever accessed
through `memcpy` — never through a pointer cast.

Privacy: the journal carries **no absolute time, no VIN, no position, no driver
identity** (VIE-01, VIE-02). All timestamps are monotonic microseconds since an
unspecified epoch, and events are also indexed by ignition cycle.

---

## 1. Partition layout

Raw partition, no file system (STO-01). Default size 48 MiB + 1 KiB.

| Offset | Size | Content |
|---|---|---|
| 0 | 512 B | allocation table, **copy A** |
| 512 | 512 B | allocation table, **copy B** |
| 1024 | 24 × 2 MiB | data segments 0..23 |

```
block_offset(seg, blk) = 1024 + seg * 2097152 + blk * 512
```

`AMBRE_BLOCKS_PER_SEGMENT` = 4096. A test may use fewer segments
(`cfg.segment_limit`), never more than 24.

Table updates always write **the copy with the lower generation**, then bump
the generation (STO-04). A reader keeps the valid copy with the **highest**
generation (REP-02). If both copies are invalid, the table is rebuilt by
scanning the segments.

## 2. Block (512 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `0x414D4252` = `"AMBR"` |
| 4 | 2 | `version` | `1` |
| 6 | 1 | `rec_type` | see §3 |
| 7 | 1 | `flags` | bit 0 = `AMBRE_FL_CONTINUATION` |
| 8 | 8 | `seq` | record sequence number, monotonic per session |
| 16 | 8 | `session_id` | boot session; stale data detector |
| 24 | 8 | `first_ts_us` | monotonic µs of the first sample |
| 32 | 2 | `chunk_index` | 0-based block index inside the record |
| 34 | 2 | `chunk_count` | blocks in the record, 1..16 |
| 36 | 4 | `payload_len` | valid bytes in this block's payload |
| 40 | 4 | `total_len` | valid bytes in the whole record payload |
| 44 | 4 | `payload_crc` | CRC-32C over `payload_len` bytes at offset 48 |
| 48 | 460 | `payload` | record body |
| 508 | 4 | `block_crc` | CRC-32C over bytes `[0, 508)` |

A record of `chunk_count > 1` occupies `chunk_count` **consecutive** blocks and
is submitted as **one contiguous write** (GEL-01). Chunk `i` holds payload
bytes `[i*460, i*460 + payload_len)`. A record is valid only if every chunk is
valid and the `total_len` bytes reassemble consistently; otherwise the whole
record is torn and must be dropped (REP-05).

CRC-32C = Castagnoli, polynomial `0x1EDC6F41` (reflected `0x82F63B78`),
initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`, no bit reflection on input
bytes beyond the reflected polynomial (the standard iSCSI/Btrfs variant).
Check value: `CRC32C("123456789") = 0xE3069283`.

## 3. Record types

| `rec_type` | Name | Payload struct | Written by |
|---|---|---|---|
| `0x01` | DATA | `ambre_sample_t[]` (8 B each) | continuous capture |
| `0x02` | EMERG | `ambre_emerg_t` (452 B) | trigger path, one write |
| `0x03` | CONT | `ambre_cont_t` (112 B) | every 10 ms during the event |
| `0x04` | SEAL | `ambre_seal_t` (32 B) | end of event / recovery |
| `0x05` | GAP | `ambre_gap_t` (20 B) | capture interruption > 3 periods |

## 4. Class B sample (8 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `signal_id` (whitelist §5) |
| 2 | 1 | `quality`: 0 valid, 1 frozen, 2 out of range, 3 sensor fault |
| 3 | 1 | `pad` = 0 |
| 4 | 4 | `value` (int32, fixed point §5) |

## 5. Signal whitelist and scaling (VIE-01, ACQ-03, ACQ-04)

Any `signal_id` outside this table is rejected and counted.

| id | Signal | Rate | Unit of `value` |
|---|---|---|---|
| 1 | acceleration X (long) | 1 kHz | mm/s² |
| 2 | acceleration Y (lat) | 1 kHz | mm/s² |
| 3 | acceleration Z (vert) | 1 kHz | mm/s² |
| 10..13 | wheel speed FL, FR, RL, RR | 100 Hz | mm/s |
| 20 | steering wheel angle | 100 Hz | 0.5 deg |
| 21 | throttle pedal | 100 Hz | 0.5 % |
| 22 | brake pedal | 100 Hz | 0/1 |
| 23 | yaw rate | 100 Hz | 0.1 deg/s |
| 24 | roll rate | 100 Hz | 0.1 deg/s |
| 25 | engine speed | 100 Hz | rpm |
| 40 | ADAS states bitmask | 10 Hz | bit0 AEB, bit1 ACC, bit2 LKA, bit3 EMG brake |
| 41 | belt states bitmask | 10 Hz | bit0 driver, bit1 front passenger, bit2 rear |
| 42 | lamp states bitmask | 10 Hz | bit0 airbag lamp, bit1 ABS lamp, bit2 ESC lamp |
| 43 | gear selector | 10 Hz | 0 P, 1 R, 2 N, 3 D, 4.. manual |
| 90..99 | **forbidden** (VIN, position, absolute time, driver id) | — | always rejected |

## 6. Class A point (20 bytes)

| Offset | Size | Field | Scale |
|---|---|---|---|
| 0 | 2 | `speed_kmh_x4` | 0.25 km/h |
| 2 | 1 | `throttle_pct` | % |
| 3 | 1 | `brake_active` | 0/1 |
| 4 | 2 | `engine_rpm` | rpm |
| 6 | 2 | `steer_angle_deg_x2` (int16) | 0.5 deg |
| 8 | 2 | `accel_long_mm_s2` (int16) | mm/s² |
| 10 | 2 | `accel_lat_mm_s2` (int16) | mm/s² |
| 12 | 1 | `abs_active` | 0/1 |
| 13 | 1 | `esc_active` | 0/1 |
| 14 | 1 | `belt_driver` | 0/1 |
| 15 | 1 | `airbag_lamp` | 0/1 |
| 16 | 2 | `yaw_rate_deg_s_x10` (int16) | 0.1 deg/s |
| 18 | 2 | `rel_time_ms` | ms before T0, 0..5000 |

`rel_time_ms` is the distance **before** T0: a point 5 s before T0 has
`rel_time_ms = 5000`, the point at T0 has `0`.

## 7. Allocation table (512 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `magic` = `0x414D5442` (`"AMTB"`) |
| 4 | 4 | `version` = 1 |
| 8 | 4 | `generation` (monotonic) |
| 12 | 4 | `flags` (bit0 = journal formatted, bit1 = memory full, bit2 = storage fault) |
| 16 | 8 | `session_id` |
| 24 | 8 | `last_seq` |
| 32 | 8 | `next_event_id` |
| 40 | 4 | `current_seg` |
| 44 | 4 | `current_block` (next free block index in `current_seg`) |
| 48 | 4 | `event_count` (entries used, ≤ 8) |
| 52 | 4 | `sealed_count` |
| 56 | 24 | `seg_states[24]` |
| 80 | 416 | `events[8]`, 52 bytes each (§8) |
| 496 | 12 | `reserved` = 0 |
| 508 | 4 | `crc` = CRC-32C over bytes `[0, 508)` |

## 8. Event descriptor (52 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `event_id` |
| 4 | 4 | `order` (monotonic; oldest first for overwrite, STO-06) |
| 8 | 1 | `state`: 0 empty, 1 open, 2 sealed |
| 9 | 1 | `locked` (never overwritten, SEC-03) |
| 10 | 1 | `file_complete` ("oui"/"non", invariant I7) |
| 11 | 1 | `trig_reason` |
| 12 | 4 | `start_seg` |
| 16 | 4 | `start_block` (block of the EMERG record) |
| 20 | 4 | `end_seg` |
| 24 | 4 | `end_block` (last accepted block, inclusive) |
| 28 | 8 | `trig_ts_us` |
| 36 | 8 | `seq_emerg` |
| 44 | 8 | `seq_last` |

## 9. Overwrite policy (STO-05, STO-06, STO-08)

1. A new event needs one free segment. Look for `AMBRE_SEG_FREE`, oldest index
   first, wrapping around the ring.
2. If none, reclaim `AMBRE_SEG_ARCHIVED` segments.
3. If still none, reclaim segments whose events are all `state = sealed` and
   `locked = 0`, from the **oldest `order`** to the newest. Such an event gets a
   SEAL record with `seal_cause = AMBRE_SEAL_OVERWRITE` before its segment is
   reused.
4. `locked` segments are never reclaimed. If nothing is reclaimable, the event
   is refused, flag `memory full` is set in the table and the refusal is
   counted (`refused_no_space`).

## 10. Recovery (REP-01..REP-05, spec 4.6)

```
1. read both table copies; keep the valid one with the highest generation
2. if neither is valid: rebuild by scanning every segment in use
   (a block is accepted while magic, version, CRCs, session_id match and seq
    continues the chain; stop at the first failure)
3. in the current segment, find the last valid block; blocks after it are
   garbage and will be overwritten (write pointer = last valid + 1)
4. drop torn records: any EMERG/CONT whose chunks are not all valid
5. an EMERG record the table does not know yet is registered (this is what
   makes the format self-describing: EMERG alone proves the event happened)
6. seal every event still open with file_complete = 0,
   seal_cause = POWERCUT, and commit the table
7. never return data whose CRC is invalid; never invent an event
```

Recovery only appends records and commits the table alternately, so it can be
interrupted by another power cut and replayed with the same final state
(REP-04, invariant I8).

## 11. Scenario file (host/scenarios)

Plain text, `#` comments, `key=value` header, then one sample per line.

```
# ambre scenario v1
name=urban-frontal
seed=1234
rate_hz=1000
duration_ms=30000
crash_at_ms=25000
# t_us a_long_mm_s2 a_lat_mm_s2 speed_kmh_x4 throttle brake rpm steer_x2 ext_flags
0 12 -3 200 0 0 800 0 0
1000 12 -3 200 0 0 800 0 0
```

`ext_flags` bitmask: bit0 restraint deployed, bit1 VRU activated,
bit2 emergency brake, bit3 AEB intervention, bit4 service command,
bit5 sample invalid (DEC-13).

Samples are 1 kHz acceleration + the slow signals repeated at their own rate
(the generator emits one line per millisecond; slow signals keep their last
value). Both the C replay producer and the Python generator must produce and
consume this exact format.

## 12. MCAP mapping (spec 6.4)

| Channel | Content | Rate | Encoding |
|---|---|---|---|
| `/edr/a/precrash` | one class A point per line | 2 Hz | `json`, schema `ambre.ClassAPoint` |
| `/edr/a/delta_v` | long/lat delta-v | 100 Hz | `json`, schema `ambre.DeltaV` |
| `/edr/a/event` | event summary | 1 per event | `json`, schema `ambre.EventSummary` |
| `/edr/b/accel` | accelerations x, y, z | 1 kHz | `json`, schema `ambre.Accel` |
| `/edr/b/vehicle` | dynamic signals | 100 Hz | `json`, schema `ambre.Vehicle` |
| `/edr/b/status` | states | 10 Hz | `json`, schema `ambre.Status` |
| `/edr/meta/gaps` | capture gaps | on demand | `json`, schema `ambre.Gap` |

T0 is placed at **60 s** in the file; no absolute timestamp appears anywhere.
Metadata records: `edr.event`, `edr.vehicle_type`, `edr.build`,
`edr.integrity`. Chunk CRC enabled, no compression, summary section and
statistics present (EXP-02).
