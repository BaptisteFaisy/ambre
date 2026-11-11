# ambre-fuzz — random-data testing of the journal recovery path

Extreme-level item of the specification (cahier des charges §12.10: *coverage of
the format library above 95 % of the lines, static analysis, **random-data
testing for 24 h***). This tool is the random-data part: it manufactures
hostile disk images and proves that the recovery path never crashes, never
loops and never breaks a documented post-condition.

It is a host tool: it runs on the PC, on the same sources that run on QNX 8.0.

## What it does

For each image:

1. **generate** a random disk image (see the families below);
2. **fork** a child that analyses it:
   `ambre_recover()` → `ambre_journal_open()` → `ambre_journal_read_event()`
   for every event the recovery declares, plus a second `ambre_recover()` that
   must change nothing (idempotency, REP-04 / invariant I8);
3. **judge** the child from the parent side:
   * a per-image wall-clock bound — exceeding it means an infinite loop and is
     a violation (the parent `SIGKILL`s the child);
   * a signal exit is a crash (segfault, ASan/UBSan abort, …) — a violation;
   * a non-zero exit code is a failed post-condition — a violation.

The child never allocates: the image lives in the parent's copy-on-write
memory, so `fork()` from the slot loop is safe.

### Image families (`host/fuzz/fuzz_gen.c`)

| family | what it simulates |
|---|---|
| `noise` | every byte random — a partition full of garbage |
| `sparse` | zeros with scattered noisy sectors, table copy randomly hit |
| `partial` | valid journal cut after *k* writes, sometimes with the last write torn at a sector boundary (partially written journal) |
| `torn-record` | valid journal whose multi-block record lost its continuation blocks (torn record) |
| `bad-table` | allocation table copy A, copy B, both, or a torn half-sector destroyed |
| `bitflip` | valid journal with 1–8 random bit flips (media error) |
| `mix` | valid journal with a run of noisy sectors |

The valid journal is produced by the real journal engine (class B stream, one
sealed and locked event, one event left open as after a power cut) on the
explorer's `ambre_memdisk_t`, so cuts replay an exact prefix of the real
write history.

## Running it

```sh
make BUILD=build-qual2 fuzz        # quick run, a few seconds (CI)
make BUILD=build-qual2 fuzz-asan   # same, built with ASan + UBSan

./build-qual2/ambre-fuzz --quick --jobs 2 --seed 20261111
./build-qual2/ambre-fuzz --minutes 10 --jobs 4
./build-qual2/ambre-fuzz --hours 24 --jobs 4 --report fuzz-24h.json
./build-qual2/ambre-fuzz --images 500 --seed 4242       # exact, reproducible
./build-qual2/ambre-fuzz --replay host/fuzz/regressions/fuzz-4242-0-check-read-event.img
```

| option | meaning |
|---|---|
| `--quick` | bounded run of 5 s (default) — for the CI |
| `--minutes N`, `--hours N` | bounded campaign (the 24 h soak of §12.10) |
| `--images N` | stop after N generated images |
| `--seed S` | base seed, default 20261111 — image *#i* depends only on `(seed, i)` |
| `--jobs N` | parallel children, 1..16 (default 2) |
| `--timeout-ms N` | per-image bound, default 5000 |
| `--regress-dir DIR` | where violating images are saved (default `host/fuzz/regressions`) |
| `--report FILE` | JSON summary of the run |
| `--replay FILE` | re-run one saved image and exit |

Exit code: `0` = no violation, `1` = at least one violation, `2` = usage error.

## Reproducibility and regressions (§7.7)

Every image is a pure function of `(seed, index)`: the same pair always
produces the same bytes and the same family. A violation is therefore saved
with its seed and is replayable:

```
host/fuzz/regressions/fuzz-<seed>-<index>-<kind>.img   the exact image
host/fuzz/regressions/fuzz-<seed>-<index>-<kind>.txt   seed, family, kind, replay command
```

`<kind>` is one of `check-<post-condition>`, `timeout`, `crash-sig<n>`.
A saved image is re-analysed with `--replay FILE`; the verdict is printed and
the exit code tells whether the image is still offending.

## Measured results (PC, this repository)

| command | result |
|---|---|
| `make coverage-gate` (build `build-cov`, gate at 95 %) | `fmt/` line coverage **98.40 %** (1349/1371) — gate **PASS** |
| `./build-qual2/ambre-fuzz --quick --jobs 2` | 75 images, **0 violation**, 14.9 img/s |
| `make BUILD=build-qual2 fuzz-asan` (ASan+UBSan) | 41 images, **0 violation**, 8.0 img/s |
| `./build-qual2/ambre-fuzz --minutes 0.1 --jobs 3 --seed 7` | 17 images, **0 violation**, 2.8 img/s |

The three violation paths (failed post-condition, infinite loop, crash
signal) and the save/replay round trip were exercised with an instrumented
copy of the analyser built outside the repository: two runs with the same
seed produced **byte-identical** images, a forced hang was killed and saved
as `timeout`, a forced `SIGSEGV` was saved as `crash-sig11`, a forced failed
check was saved as `check-read-event`, and `--replay` of a saved image
reported `OK` when the image itself is clean.

## Files

| file | role |
|---|---|
| `fuzz_journal.c` | campaign loop: slots, fork, timeout, report, replay |
| `fuzz_cli.c` | argument handling, violation saving |
| `fuzz_gen.c` | image families (the generator) |
| `fuzz_child.c` | the post-conditions checked inside the child |
| `fuzz_rng.h` | splitmix64, the deterministic PRNG |
| `fuzz_run.h` | shared types |

## Limits

* one image is the full journal size (48 MiB), so the throughput is bounded
  by image generation (~5 images/s per job on the development PC);
* the generator is random, not coverage-guided: it does not aim at the
  branches the tests already cover;
* the per-image bound is wall-clock, not instruction-count: on a heavily
  loaded machine a slow image may be killed before the real limit is reached
  (raise `--timeout-ms` in that case);
* the tool checks the *portable* code only; the QNX glue is covered by the
  syntax check and by the target tests, not by this fuzzer.
