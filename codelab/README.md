# Codelab — Writing a journal that survives power cuts on QNX 8.0, and proving it

**Audience:** a developer who knows C and has never written a crash-tolerant
log.

**What you will build:** a small embedded event recorder — an EDR "black box" —
that writes to a raw partition on QNX 8.0, loses power at the worst possible
moment, and still comes back with the data it had already promised to keep.

**What you will prove:** not with a story, but with an exhaustive exploration of
every disk state a power cut can leave behind. The claim "no data is ever lost"
is turned into a finite set of states that a program enumerates and checks.

**Time:** the six steps below are the six pieces of `fmt/` and `host/`, in the order you need
them. Each step names the file you write and the test that proves it. **Where things are:**
the binary format contract is `docs/FORMAT.md`, the architecture `docs/ARCHITECTURE.md`, the
validation plan `docs/VALIDATION.md`; the portable core is `fmt/`, the simulated disk
`host/simdisk/`, the explorer `host/explore/`, the tests `tests/`.

---

## Step 0 — The problem, in one sentence

Your program must write a record to a disk, and the power may be cut *between
the moment you call `write()` and the moment the bytes are actually on the
medium*. Everything in this tutorial follows from that sentence. Three things
follow immediately:

1. **A record is not a byte stream.** It has a header, a payload and a checksum,
   because the medium may keep *part* of what you wrote. You need to be able
   to say "this half of the record is garbage".
2. **You need two copies of the metadata.** The metadata says where the data
   is. If the power dies while you update it, you must still have the previous
   version. Hence a table with two copies and a generation counter.
3. **You need to know what was durable.** Without that knowledge you cannot
   even state the guarantee, let alone test it. Hence the simulated disk that
   logs every write and every flush.

---

## Step 1 — The block format and the CRC

**You write:** `fmt/ambre_crc32c.c`, `fmt/ambre_fmt_block.c`
**You read:** `docs/FORMAT.md` §2 (the normative field list)
**It proves:** `tests/test_crc32c.c`, `tests/test_block.c`

### 1.1 Why a checksum, and which one

A block that is half-written is not obviously half-written: you may read back
512 bytes that *look* fine. You need a cheap test that says "these bytes are
the bytes I meant to write".

Ambre uses **CRC-32C** (Castagnoli, the iSCSI/Btrfs variant): reflected
polynomial `0x82F63B78`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`.
The check value is fixed and public:

```
CRC32C("123456789") == 0xE3069283
```

Put that value in your test suite *before* you write the rest of the algorithm.
If your CRC does not produce it, stop: every later failure will be unreadable.

The CRC is computed **incrementally**, in the classic `crc32()` chaining
style: the caller passes `0` for the first chunk, then feeds back the previous
return value. The journal writes a block at a time but sometimes needs to
checksum a payload that was assembled in pieces; a one-shot call over a
contiguous buffer would force a copy on the critical path. The whole API is
one function:

```c
uint32_t ambre_crc32c(uint32_t crc, const void *buf, size_t len);
```

so `ambre_crc32c(0, p, n)` is the CRC of `p`, and
`ambre_crc32c(ambre_crc32c(0, p, 7), p + 7, n - 7)` is the same value. That
equivalence — chunked, and even byte by byte — is one of the test cases.

### 1.2 The block

A block is exactly **one sector**: 512 bytes, little-endian, packed, no
padding. It is never accessed through a pointer cast — always through `memcpy`
— so the code is portable and free of alignment faults on QNX.

```
offset 0    magic        "AMBR" (0x414D4252)   -> rejects foreign data
offset 4    version      1
offset 6    rec_type     DATA / EMERG / CONT / SEAL / GAP
offset 7    flags        bit 0 = continuation
offset 8    seq          64-bit monotonic sequence number
offset 16   session_id   boot session; stale-data detector
offset 24   first_ts_us  monotonic microseconds of the first sample
offset 32   chunk_index  0-based position inside the record
offset 34   chunk_count  blocks in the record, 1..16
offset 36   payload_len  valid bytes in this block
offset 40   total_len    valid bytes in the whole record
offset 44   payload_crc  CRC-32C over the payload
offset 48   payload      460 bytes of record body
offset 508  block_crc    CRC-32C over bytes [0, 508)
```

Two CRCs, on purpose: `payload_crc` tells you the *body* is intact, `block_crc`
tells you the *header* is intact. If the header is damaged you cannot even
trust `payload_len`, so you must not use it.

### 1.3 The rules that make a torn record detectable

- A record longer than one block occupies **consecutive** blocks and is
  submitted as **one contiguous write**. The medium then sees one transaction
  rather than a sequence of sector writes.
- `chunk_index` / `chunk_count` say where each block belongs. On read, you
  gather the chunks, check that the bytes reassemble to `total_len`, and check
  every CRC.
- **A record is valid only if every chunk is valid.** Otherwise the whole
  record is torn and is dropped. Half an event is worse than no event: it
  would be reported as data that was never fully recorded.

### 1.4 The test that proves it

`tests/test_crc32c.c`: `crc32c_check_value` (the public check value),
`crc32c_empty_buffer`, `crc32c_incremental_vs_oneshot` (the incremental form
equals the one-shot), `crc32c_single_bit_flip`.

`tests/test_block.c`: `block_pack_unpack_roundtrip`,
`block_corruption_error_codes` (a bad magic, a bad version and a bad CRC each
give a *distinct* error code), `block_invalid_lengths_and_chunks`,
`block_multiblock_reassembly_io` (a multi-block record survives a write/read
cycle and reassembles), `block_offset_segblk_roundtrip`.

```sh
make all && ./build/test_crc32c && ./build/test_block
```

---

## Step 2 — The table in two copies

**You write:** `fmt/ambre_fmt_table.c`
**You read:** `docs/FORMAT.md` §7 (the table layout), §1 (the partition layout)
**It proves:** `tests/test_table.c`

### 2.1 The layout

The partition is 48 MiB and has no file system:

```
offset 0      copy A of the allocation table   (512 B)
offset 512    copy B of the allocation table   (512 B)
offset 1024   data segments 0..23, 2 MiB each

block_offset(seg, blk) = 1024 + seg * 2097152 + blk * 512
```

The table says, for each of the 24 segments, whether it is `FREE`, `CURRENT`,
`FROZEN` (pinned by an acquired event), `LOCKED` (holds a locked event, never
reused) or `ARCHIVED`; and for each of the 8 events, its id, its state, its
locked flag, its `file_complete` flag, the segment and block of its EMERG
record, and its sequence numbers.

### 2.2 Why two copies

The table is the map of the partition. If you lose it while updating it, you
lose the map — and the data becomes unreachable even though it is still
physically there.

So: two copies, a monotonic `generation` counter, a CRC on each copy.

- **A commit writes only the copy with the *lower* generation**, then bumps
  the generation (STO-04). At any moment, one copy is the live one and the
  other is the previous version.
- **A reader keeps the valid copy with the *highest* generation** (REP-02).
- If neither copy is valid, the table is rebuilt by scanning the segments
  (REP-02 fallback, step 5 of the recovery).

The rule "write the older copy" is what makes this work: the power can die
during the write of the older copy, and the newer copy — untouched — is still
there.

### 2.3 The test that proves it

`tests/test_table.c`: `table_init_and_validate` (a fresh table validates),
`table_read_best_highest_gen` (with two valid copies, the highest generation
wins), `table_commit_alternates_corrupt` (a commit alternates the copy it
writes; corrupting the copy that was just written leaves the other usable),
`table_pick_victim_policy` (the overwrite policy picks the oldest unlocked
segment, never a locked one).

```sh
make all && ./build/test_table
```

---

## Step 3 — The continuous journal

**You write:** `fmt/ambre_journal_core.c` + `fmt/ambre_journal_event.c` (engine), `fmt/ambre_fmt_scan.c`, `fmt/ambre_fmt_record.c`
**You read:** `fmt/ambre_journal.h` (the engine's contract)
**It proves:** `tests/test_simdisk.c`, `tests/test_scan.c`, `tests/test_journal.c`

### 3.1 The simulated disk comes first

Before writing the journal, write the thing that makes the whole tutorial
possible: a disk that lies to you.

`host/simdisk/ambre_simdisk.c` implements the same four operations as the
real medium — `read`, `write`, `flush`, and an optional `mark` — but it keeps
a **log of every operation in order**. That log is the ground truth: it tells
you, for any cut point, exactly what was durable. On top of it:

- `ambre_simdisk_snapshot(sd, prefix, var, dst, dst_size)` — image of the disk after the first
  `prefix` operations, with an optional cut variant (`ambre_cut_variant_t`, may be NULL):
  `torn_last_write`+`torn_bytes` (last write torn at a sector boundary), `drop_unflushed`+
  `keep_mask` (a volatile cache losing unflushed writes), `corrupt_bit`+`corrupt_off`+
  `corrupt_bit_index` (one flipped bit, ROB-07), `wipe_table_a`/`wipe_both_tables` (forcing
  the scan-rebuild path). This is "the disk as it looked when the power died".
- `ambre_simdisk_apply_prefix(sd, prefix)` — same, in place: the explorer's fast path (one
  snapshot per prefix, then cheap variants).

The `mark` operation is the journal's way of saying "this milestone is
reached": `AMBRE_MARK_EMERG_WRITTEN`, `AMBRE_MARK_ACQUIRED`,
`AMBRE_MARK_SEALED`, `AMBRE_MARK_DATA_FLUSHED`, `AMBRE_MARK_TABLE_COMMIT`.
The marks turn the operation log into a timeline with named instants.

### 3.2 The journal engine

The engine appends records. In continuous mode it appends `DATA` records
(class B samples) and flushes at a fixed cadence (100 ms by default). It
never rewrites a block: an acknowledged block is not rewritten before its
segment is released (STO-03).

The engine state is a **fixed-size struct** with pre-allocated staging
buffers. No `malloc` anywhere on the write path. That is not an optimisation
detail — it is requirement GEL-03, and it is what lets step 4 promise a
bounded latency.

### 3.3 The scan

`fmt/ambre_fmt_scan.c` walks a segment from a given block, following the
sequence numbers, and stops at the first block that fails magic, version,
CRC, session or seq continuity. It reports the last valid block, the next
free block, and whether it met an `EMERG` record.
This scan is the fallback path of the recovery: if both table copies are
unusable, the partition is rebuilt from what the scan finds.

### 3.4 The test that proves it

`tests/test_simdisk.c` — `create_destroy`, `write_read_roundtrip`,
`logging_exactness`, `find_mark_and_last_flush`, `prefix_and_idempotency`,
`snapshot_torn_write`, `snapshot_volatile_cache`,
`snapshot_corruption_tables`, `overlapping_writes`, `bounds_and_overflow`,
`restore_image`, `read_logging_gated`, `write_payload_pool`.

`tests/test_scan.c` — `scan_valid_chain_and_emerg`,
`scan_stops_on_crc_seq_session`, `scan_rebuild_table_exact_events`.

```sh
make all && ./build/test_simdisk && ./build/test_scan
```
---

## Step 4 — The critical path of the freeze

**You write:** the trigger path of `fmt/ambre_journal_event.c` (arithmetic in `fmt/ambre_trigger.c` + `fmt/ambre_trigger_window.c`)
**You read:** `fmt/ambre_journal.h` (the durability sequence), `docs/ARCHITECTURE.md` §2
**It proves:** `tests/test_explore.c::explore_acquisition_boundary`,
invariant I2 and I5 of the exploration, and the latency campaign
(`TODO(campaign)`)

This is the heart of the tutorial. Everything above was preparation.

### 4.1 The deadline

From the instant the trigger fires to the instant the event is **acquired**,
the target is **20 ms** — and 500 µs of that must be software. The trigger
happens at the worst possible moment: the engine of the car is being smashed.
You have 20 ms to get the class A pre-crash snapshot onto the medium.

### 4.2 Three steps, no more

```
trigger fires
     |
     v
1. ONE contiguous write of the EMERG record
     |
     v
2. ONE allocation-table commit (older copy, generation + 1)
     |
     v
3. ONE disk flush
     |
     v
on flush acknowledgement -> the event is ACQUIRED
```

Three properties make this correct:

- **The order is not arbitrary.** The EMERG record *is* the proof that the
  event happened; the table merely describes it. Write the record first: if
  the power dies between the two, recovery finds an EMERG record the table
  does not know about and registers it (this is what makes the format
  self-describing). The reverse order would leave a table entry pointing at
  data that never arrived.
- **"Acquired" means "flushed", never "submitted"** (GEL-02). The flush
  acknowledgement is the only durable point on the path.
- **Nothing else runs on the path** (GEL-03): no allocation, no page fault,  no wait on a lower-priority process. The class A snapshot is copied into a
  pre-allocated buffer and written from there.

### 4.3 Why not write into a file system?

On the QNX 8.0 Power-Safe file system, `fsync()` synchronises the **whole
file system**, not your file, and is documented as "potentially expensive";
`O_SYNC` is **ignored**. A single data modification can trigger the copy of a
dozen blocks.

That is *excellent* for consistency and *hopeless* for a 20 ms deadline: the
cost is not bounded by the size of your write. So the journal lives on a
**raw partition**, where the flush order reaches the medium as a flush order,
and the MCAP archive — the part that wants a file system, and that can afford
to wait — lives on the Power-Safe partition, built later by a low-priority
process. (Whether a flush on a raw partition really reaches the virtual disk
is essay E1, `host/powercut/e1_flush_probe.py`. Its result is
`TODO(campaign)`.)

### 4.4 After the acquisition

Only once the event is acquired does the during-event cadence start: one
`CONT` record plus one flush every 10 ms (GEL-04). At the end of the event, a
`SEAL` record carries the maximum delta-v, its instant, the restraint
deployment times and `file_complete = yes` (GEL-05). No archive write happens
while an event is open (GEL-06).

### 4.5 The test that proves it

The acquisition boundary is the interesting instant: one microsecond earlier,
the event does not exist; one microsecond later, it must exist in full. The
explorer walks the whole operation log, so it crosses that boundary at every
possible operation — that is what `explore_acquisition_boundary` checks. The
measured latency is a separate matter, and is still open:
`campaign:latency` — `TODO(campaign)`.

```sh
make all && ./build/test_explore
```

---

## Step 5 — Idempotent recovery

**You write:** `fmt/ambre_fmt_recovery.c`
**You read:** `docs/FORMAT.md` §10 (the recovery procedure)
**It proves:** `tests/test_explore.c::explore_double_recovery`,
`explore_no_crash_no_event`, `explore_bitflip_detected`,
`explore_table_destruction`, invariants I1, I3, I7, I8

### 5.1 The procedure

Recovery runs **before any new capture** (REP-01):

1. Read both table copies; keep the valid one with the highest generation.
2. If neither is valid, rebuild the table by scanning every segment in use.
3. In the current segment, find the last valid block; everything after it is
   garbage and will be overwritten.
4. Drop torn records: any EMERG/CONT whose chunks are not all valid.
5. Register an EMERG record the table does not know yet — an EMERG alone
   proves the event happened.
6. Seal every event still open with `file_complete = no`, cause `POWERCUT`,
   then commit the table.
7. Never return data whose CRC is invalid; never invent an event.

### 5.2 Why it is idempotent

Step 5 is the subtle one. After a power cut, the table may be *behind* the
data: the EMERG record reached the medium but the table commit did not. If
recovery only trusted the table, that event would be lost — even though the
medium holds it.

So the format is **self-describing**: the record wins over the table. An EMERG
record found on the medium is enough to register the event.

And because the recovery only *appends* records and *alternates* table
updates, running it twice gives the same result as running it once, and
interrupting it with a new power cut and running it again gives the same
result too (REP-04, invariant I8). That is what makes the system safe to
retry after every crash, forever.

### 5.3 The tests that prove it

- `explore_double_recovery` — two recoveries in a row, same final table.
- `explore_no_crash_no_event` — a cut before the trigger leaves no event.
- `explore_bitflip_detected` — a flipped bit is detected, the neighbours are
  untouched.
- `explore_table_destruction` — both table copies destroyed: the table is
  rebuilt by scanning.

```sh
make all && ./build/test_explore
```

---

## Step 6 — Exhaustive exploration of the cuts

**You write:** `host/explore/`: `ambre_explore.c`, `ambre_explore_gen.c`, `ambre_explore_run.c`, `ambre_explore_worker.c`, `ambre_invariants.c`, `ambre_memdisk.c`, `ambre_scenario.c`, `main.c`
**You read:** `docs/VALIDATION.md` §2 (the eight invariants)
**It proves:** the whole thing. This is the proof machine.

### 6.1 The idea

"Survives a power cut" sounds like an infinite claim. It is not: the journal
performs a **finite** sequence of operations. Cut the power after operation
*k*, for *k* from 0 to *N*, and you have a finite set of possible disk
states — a few more once you model a torn write and a volatile cache.

So: enumerate them all, recover from each, and check the invariants. If the
enumeration is complete, the claim is proved for *every* cut instant, not
sampled.

### 6.2 The pipeline

1. **Record.** Run the journal on the simulated disk over a scenario, with the
   operation log and the marks. This produces both the operation log and the
   **ground truth**: the samples that were injected, and the byte-exact class A
   snapshot of each event.
2. **Generate.** For each prefix of the log, build the disk image. Add the
   variants: torn last write, dropped unflushed writes, one flipped bit,
   destroyed table copies.
3. **Recover.** Run the recovery on the image.
4. **Check.** Verify the eight invariants **against the injected ground
   truth** — never against the recovered data alone. A recovery that invents
   state is caught precisely because the truth comes from the input side.
5. **Nest.** Replay the same exploration on the *recovery's own trace*
   (invariant I8): a cut during recovery, then a new recovery.

### 6.3 The eight invariants

| ID | Invariant |
|---|---|
| I1 | Recovery terminates, in a bounded number of steps, without crashing. |
| I2 | Every event acquired before the cut is present, and its pre-crash class A data is identical to the injected data. |
| I3 | No event appears that was not triggered. |
| I4 | Every restored sample is identical to the injected sample of the same instant. |
| I5 | For an acquired event, the last restored sample is less than one flush period plus one block old at the moment of the cut. |
| I6 | A locked event survives any subsequent sequence of events and cuts. |
| I7 | "File complete" is "yes" if and only if the event was sealed before the cut. |
| I8 | A cut during recovery or during archiving, followed by a new recovery, gives the same final state. |

The checker is `host/explore/ambre_invariants.c`. It sets one bit per
violated invariant and keeps an actionable message: event id, prefix, expected
and found values. A violation is never a lost failure — the disk image that
reproduces it is kept and becomes a non-regression test.

### 6.4 Running it

```sh
make scenario     # build/scenario.txt, seed 1234
make explore      # 10 000 states, all variants
make explore-full # every reachable state (64 591 measured), nested included

# or directly:
build/ambre-explore --scenario build/scenario.txt \
  --states 10000 --jobs "$(nproc)" --variants all \
  --report build/explore.json
```

The explorer prints the number of states explored, the rate and the first
violation, and returns 0 only when no invariant was violated. The JSON report
carries `states_explored`, `violations`, `states_by_variant` and
`violations_by_invariant`. Measured 2026-11-11 on the reference 30-second
scenario: the trace yields **5 803 prefix states**; with variants and nested
replay the reachable space is **64 591 states** (prefix 5 803, torn 14 336,
cache 21 240, bitflip 5 803, table 11 606, nested 5 803) — **0 violations**,
334 s at 193 states/s with `--jobs 10`; `make explore` (10 000 states) takes
29 s. `make explore-full`'s `--states 1000000` is an upper bound the reachable
space never reaches: the guarantee is enumerating *every* state, not a round number.

### 6.5 Where it stops, and where it continues

The PC explorer proves the *logic*. It does not prove that QNX emits the flush
orders, nor that the hardware behaves. That is the next level: real cuts
under QEMU (`host/powercut/`), then real electrical cuts on a Raspberry Pi 5.
The limitation is honest and stated: killing QEMU does not lose the writes
already handed to the host. Level 1 covers the volatile-cache loss; level 2
covers the real chain.

---

## Where to go from here

- Add a file system? Do not, on the critical path. Read §4.3 again.
- Want the class A regulatory points? `docs/FORMAT.md` §6.
- Want the export? `host/mcap/export_mcap.py` and `docs/FORMAT.md` §12.
- Want the whole gate in one command? `./scripts/verify-all.sh`.
- Want the demo? `./scripts/demo.sh`.

**The one sentence to remember:** *the medium may keep part of what you wrote;
so write records that are individually verifiable, keep two copies of the
map, and know exactly which of them reached the disk.*
