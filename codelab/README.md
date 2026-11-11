# Codelab — Writing a journal that survives power cuts on QNX 8.0, and proving it

**Audience:** a developer who knows C and has never written a crash-tolerant
log.

**What you build:** a small embedded event recorder — an automotive EDR, a
"black box" — whose journal is a raw partition on QNX 8.0. It loses power at
the worst possible moment and still comes back with the data it had already
promised to keep.

**What you prove:** not with a story, but with an **exhaustive exploration** of
every disk state a power cut can leave behind. "No data is ever lost" becomes
a finite set of states that a program enumerates and checks, and whose
violation count is zero.

**Time:** about 2 to 3 hours to read and run everything. Each chapter is
15–30 minutes: 10 to read, 5 to run, 5 to look at what the test actually
asserts. Chapter 6 adds machine time: `make explore` takes 29 s on an idle
machine (77 s if other campaigns are running) and `make explore-full` 334 s to
495 s depending on the load — start them and read while they run.

---

## Prerequisites

| Need | Why | Check |
|---|---|---|
| `gcc` ≥ 11 and `g++` (C++17) | the portable library, the tests, the C++ archive | `gcc --version` |
| `make` | one build for PC and (later) for QNX | `make --version` |
| `python3` ≥ 3.9, standard library only | scenario generator, MCAP exporter, verifiers | `python3 --version` |
| ~500 MB of disk | 30 s scenarios, journal images, build trees | `df -h .` |
| A POSIX machine (Linux or macOS) | the PC backend of the portability layer | — |

**Not required:** a QNX SDP. Nothing in this codelab needs one. Everything runs
on the PC against the POSIX backend, and the QNX-specific code is only
*syntax-checked* against stub headers (`make qnx-syntax`). What that proves —
and what it does not — is stated in [`../docs/QNX-PORT.md`](../docs/QNX-PORT.md).
Be suspicious of any tutorial that hides that distinction; this one does not.

**Conventions used throughout:**

- One private build directory per reader, never the shared `build/`:
  `make BUILD=build-mine …`. Parallel readers on the same tree will otherwise
  overwrite each other's artefacts.
- Every test binary returns **the number of failed checks** as its exit code,
  so `0` means green and a number tells you how much is broken.
- Every measured figure quoted in the chapters was produced by the command
  printed next to it, on this repository, on 2026-11-11. Slots that were never
  measured say so.

---

## The six chapters

Read them in order: each one is the piece the next one assumes.

| # | Chapter | You build | Proves | Run |
|---|---|---|---|---|
| 1 | [The block format and CRC-32C](01-block-and-crc.md) | `fmt/ambre_crc32c.c`, `fmt/ambre_fmt_block.c` | STO-02, REP-05 | `./build-mine/test_crc32c`, `test_block` (85 + 118 checks) |
| 2 | [The table in two copies](02-table-two-copies.md) | `fmt/ambre_fmt_table.c` | STO-03, STO-04, REP-02 | `./build-mine/test_table`, `test_scan` (44 + 68) |
| 3 | [The continuous journal](03-continuous-journal.md) | `fmt/ambre_journal_core.c`, `fmt/ambre_fmt_scan.c` | ACQ-06, STO-01, STO-03 | `./build-mine/test_simdisk`, `test_scan`, `test_journal` (3 345 + 68 + 4 195) |
| 4 | [The critical path of the freeze](04-freeze-critical-path.md) | the trigger path, `fmt/ambre_journal_event.c` | GEL-01, GEL-02, GEL-03 | `./build-mine/test_journal`, `test_explore` (4 195 + 58) |
| 5 | [Idempotent recovery](05-idempotent-recovery.md) | `fmt/ambre_fmt_recovery.c` | REP-01..REP-05, I1/I3/I7/I8 | `./build-mine/test_recovery`, `test_explore` (124 + 58) |
| 6 | [Exhaustive exploration of the cuts](06-exhaustive-exploration.md) | `host/explore/` | ROB-01, the whole of §7.3 | `make explore` (10 000 states), `make explore-full` (64 591 states) |

Requirement ids (ACQ, GEL, STO, REP, ROB) are those of the specification,
[`../docs/cahier-des-charges.md`](../docs/cahier-des-charges.md) §5; the
invariants I1..I8 are §7.2. The traceability matrix — every level-S
requirement to the test or campaign that covers it — is
[`../docs/VALIDATION.md`](../docs/VALIDATION.md) §5.

---

## Before chapter 1 — the problem in one sentence

Your program must write a record to a disk, and the power may be cut *between
the moment you call `write()` and the moment the bytes are actually on the
medium*. Everything in this codelab follows from that sentence:

1. **A record is not a byte stream.** It has a header, a payload and a
   checksum, because the medium may keep *part* of what you wrote. You must be
   able to say "this half of the record is garbage" (chapter 1).
2. **You need two copies of the metadata.** The metadata says where the data
   is. If the power dies while you update it, the previous version must still
   be there (chapter 2).
3. **You need to know what was durable.** Without that you cannot even state
   the guarantee, let alone test it — hence a simulated disk that logs every
   write and every flush (chapter 3).
4. **The hard deadline is at the worst possible instant.** "Durable" has to fit
   in twenty milliseconds after the impact (chapter 4).
5. **Recovery runs after the failure it handles, so it will be interrupted
   too.** It must be safe on garbage and replayable (chapter 5).
6. **A claim about "any instant" needs an enumeration, not twenty reboots**
   (chapter 6).

---

## Get the tree in a known-good state

```sh
cd ambre
make BUILD=build-mine all          # library, C tools, the five process binaries
make BUILD=build-mine test         # every C suite: exit code = failures
```

`make BUILD=build-mine test` ends with `ALL TESTS PASSED` when green. If you
want the single gate that runs everything (size check, build, C and Python
suites, generator, exploration, MCAP round-trip):

```sh
AMBRE_BUILD_DIR=build-mine ./scripts/verify-all.sh --quick
```

Useful references while reading: [`../docs/FORMAT.md`](../docs/FORMAT.md) (the
normative binary format), [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md)
(the five processes), [`../docs/CAMPAIGN.md`](../docs/CAMPAIGN.md) (every
measured number, with the command that produced it).

---

## Where to go from here

- **Add a file system on the critical path?** No. Re-read chapter 4 §4.6:
  Power-Safe `fsync()` is volume-wide and `O_SYNC` is ignored.
- **Want the regulatory Class A points?** [`../docs/FORMAT.md`](../docs/FORMAT.md) §6.
- **Want the archive and the viewer?** `host/mcap/export_mcap.py`, then the
  Foxglove layout in `foxglove/`.
- **Want the whole gate in one command?** `./scripts/verify-all.sh`.
- **Want the end-to-end demo?** `make demo`.
- **Want to know what is *not* proven?** [`../docs/CAMPAIGN.md`](../docs/CAMPAIGN.md)
  §8 and [`../docs/QNX-PORT.md`](../docs/QNX-PORT.md). Read them before you
  quote any number from this codelab.

**The one sentence to remember:** *the medium may keep part of what you wrote;
so write records that are individually verifiable, keep two copies of the map,
and know exactly which of them reached the disk.*
