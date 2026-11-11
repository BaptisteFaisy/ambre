# Weeks 2–5 plan, file allocation and frozen interfaces

This file is the contract for the parallel work on `target/`, `image/`,
`host/` and `docs/`. Rules from `docs/AGENT-BRIEF.md` apply unchanged: files
under 500 lines, functions under 50 lines, no duplication, own build directory
(`build-<suffix>`), never `git commit`, never delegate, never touch a file
assigned to another agent.

## Layering

```
fmt/            portable journal format + recovery   (DONE, week 1, 8070 checks)
target/port/    portability layer                    (contract frozen here)
target/ipc/     wire protocol helpers                (target/edr_ipc.h frozen)
target/<proc>/  portable process logic  +  <proc>_qnx.c glue (QNX only)
host/           PC tooling: exploration, campaigns, reports
image/          mkqnximage fragments + security policy
```

The rule that makes this work without a QNX SDP: **all logic is portable and
tested on the PC; only `*_qnx.c` files include QNX headers**, and those are
compiled against `target/qnx-stub/include/` (syntax check, never linked, never
executed). See `docs/QNX-PORT.md` for what each build proves.

## Frozen: portability layer

`target/port/ambre_port.h` is frozen. It is implemented by
`ambre_port_posix.c`, `ambre_port_posix_disk.c`, `ambre_port_posix_ipc.c`
(done) and by `ambre_port_qnx.c` (QNX glue, agent qnx-glue).

## Frozen: process command lines

Every process takes its endpoints on the command line, so the same binaries
run under a test harness on the PC and from an image buildfile on QNX.

```
edr-journal  --disk <file|/dev/hd0tNNN> --listen <sock> [--mode continuous|event]
             [--segment-limit N] [--session-id N] [--flush-us N] [--event-flush-us N]
edr-capture  --journal <sock> --listen <sock> [--profile eu|us]
edr-replay   --capture <sock> --scenario <file.txt> [--rate 1.0 | --fast]
             [--kill-after-ms N]
edr-archive  --journal <sock> --out <dir> [--once]
edrctl       --journal <sock> {status|list|export <id> --out <f.mcap>|verify}
             [--json]
```

Exit codes: 0 ok, 1 functional failure, 2 usage error. Every process logs
through `ambre_port_log()` and honours `AMBRE_LOG=<0..3>`.

## Frozen: message protocol

`target/edr_ipc.h` is frozen (header 16 bytes, magic `EDR1`, message types,
bodies). Helpers `ambre_ipc_header_init()` / `ambre_ipc_header_validate()` are
implemented in `target/ipc/edr_ipc.c` (agent port-ipc).

## File allocation

| Agent | Files it owns |
|---|---|
| port-ipc | `target/ipc/edr_ipc.c`, `tests/test_ipc.c`, `tests/test_port_posix.c` |
| capture | `target/capture/edr_capture.h`, `target/capture/edr_capture.c`, `target/capture/main.c`, `tests/test_capture.c` |
| journal-svc | `target/journal/edr_journal_svc.h`, `target/journal/edr_journal_svc.c`, `target/journal/main.c`, `tests/test_journal_svc.c` |
| qnx-glue | `target/port/ambre_port_qnx.c`, `target/journal/edr_journal_qnx.c`, `target/capture/edr_capture_qnx.c`, `target/qnx-stub/include/**` (extend only), `docs/QNX-ABI.md`, Makefile target `qnx-syntax` |
| archive-cpp | `target/archive/mcap_writer.hpp`, `target/archive/mcap_writer.cpp`, `target/archive/edr_archive.cpp`, `target/archive/main.cpp`, `tests/test_archive_cpp.cpp` |
| ctl-replay | `target/ctl/edrctl.c`, `target/replay/edr_replay.c`, `tests/test_edrctl.c` |
| image-sec | `image/ambre.build`, `image/ambre.sec`, `image/buildfile`, `image/README.md`, `docs/SECURITY.md` |
| e2e-cuts | `host/e2e/**`, `tests/test_e2e_posix.c`, `host/processcut/**`, `scripts/demo.sh` |
| latency | `host/latency/**`, `docs/CAMPAIGN-LATENCY.md` |
| quality | `scripts/coverage.sh`, `host/fuzz/**`, `host/energy/**`, `host/report/**` |
| hardening | `tests/test_policy.c`, `host/data/**`, `docs/DATA.md` |
| docs | `codelab/**`, `docs/CAMPAIGN.md`, `README.md` (status tables only) |

## Definition of done per week

- **Week 2** — the five processes exist, the portable halves are tested on the
  PC, the QNX glue compiles against the stubs, and one event travels
  replay → capture → journal → disk and survives a `SIGKILL` + restart.
- **Week 3** — MCAP archive in C++17 verified by the Python reader, startup
  recovery, and the cut campaign reaches the socle threshold (200 cuts, zero
  loss) on the PC backend, labelled as such.
- **Week 4** — locking, multi-event, overwrite, access-control decisions,
  supervision, and the latency campaign (software share measured, media share
  marked not measured).
- **Week 5** — README, tutorial, campaign results, and at least two extreme
  items from spec §12 measured. No video.
