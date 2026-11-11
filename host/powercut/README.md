# Power-cut validation level (spec §7.4) — tools

Level 2 of the validation plan: **real cuts**. A QEMU machine runs the EDR,
its power is cut at a chosen instant, the machine restarts, the exported events
are compared with what was injected. This directory holds the orchestrator, its
feasibility harnesses (essays E1 and E3) and a faulty block server that lifts
the known limitation of level 2.

Python 3 standard library only, no network, no QNX image required to run the
tools: everything works with `--backend dry-run`, a fake guest process that
behaves like a booted machine.

| File | What it does |
|---|---|
| `qemu_vm.py` | one QEMU machine: command line, bounded readiness probe, guest commands, power cut (`SIGKILL`), restart, cycle timing. Shared helpers (`wait_for`, `parse_events`, `terminate_process`) live here — no duplicated wait/probe logic anywhere else. |
| `powercut_campaign.py` | the campaign: sweep of cut instants, cut/restart/collect/compare/classify, JSON report, text histogram, violation file, `--jobs`, `--resume`. |
| `e1_flush_probe.py` | essay E1: does a write + flush on a raw partition reach the virtual disk as a FLUSH order? Host mode measures it today through the block server; guest mode holds the exact QNX commands for when the SDP arrives. Also hosts the reference `NbdClient`. |
| `e3_cycle_timing.py` | essay E3: kill + restart + service again, N cycles; answers "less than ten seconds per cycle?" with min / median / p99 / max. |
| `faulty_block_server.py` | NBD block server with a simulated volatile cache: unflushed writes are dropped at the cut, FLUSH orders are journalled. |
| `PROTOCOL.md` | wire protocol and cache semantics of the block server. |

## Quick start (no QNX needed)

```sh
cd /home/baptiste/Bureau/ambre

# 1. Test suite: 18 tests, well under the 5-minute CI budget.
python3 tests/test_powercut_py.py          # exit code = number of failures

# 2. Essay E3 with the fake guest.
python3 host/powercut/e3_cycle_timing.py --backend dry-run --cycles 5

# 3. Essay E1, host mode: write 4 KiB, flush, check the FLUSH was journalled.
python3 host/powercut/e1_flush_probe.py --mode host

# 4. A five-cut campaign, dry-run.
cat > /tmp/scenario.txt <<'EOF'
duration_ms 300
inject 0 evt-a
acquire 20 evt-a
inject 60 evt-b
acquire 80 evt-b
EOF
python3 host/powercut/powercut_campaign.py --scenario /tmp/scenario.txt \
    --backend dry-run --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 50 \
    --report /tmp/report.json
```

## Running the campaign when the QNX 8.0 image is available

The image is a raw disk image with QNX 8.0 installed, the EDR running as a
resource manager and `edrctl` in `PATH` of the guest shell, with the shell on
the serial console.

```sh
cd /home/baptiste/Bureau/ambre

# 1. Sanity check: the machine boots and the console answers.
timeout 300 qemu-system-x86_64 -machine q35 -accel tcg -m 512 -smp 2 \
    -display none -no-reboot -net none \
    -drive file=qnx80.img,if=none,id=ambre,format=raw,cache=none,aio=native \
    -device virtio-blk-pci,drive=ambre \
    -serial unix:/tmp/ambre-console.sock,server=on,wait=off

# 2. Essays first (they decide the parameters of the campaign).
python3 host/powercut/e3_cycle_timing.py --image qnx80.img --cycles 20 \
    --report /tmp/e3.json
python3 host/powercut/e1_flush_probe.py --mode guest --image qnx80.img \
    --report /tmp/e1.json

# 3. The full sweep: 551 instants, one per millisecond, -50 ms to +500 ms.
python3 host/powercut/powercut_campaign.py --image qnx80.img \
    --scenario host/scenarios/crash-50kmh.txt \
    --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 1 \
    --report /tmp/campaign.json --violation-file /tmp/violations.jsonl

# 4. The same sweep with random instants and 4 machines in parallel (socle: 200
#    cuts; complet: 2000). Resumable: interrupt and re-run with --resume.
python3 host/powercut/powercut_campaign.py --image qnx80.img \
    --scenario host/scenarios/crash-50kmh.txt \
    --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 1 --random-cuts 1500 \
    --jobs 4 --cycles 1 --resume \
    --report /tmp/campaign.json

# 5. Extreme level: the disk is served by the faulty block server, so a cut can
#    really lose a volatile cache.
python3 host/powercut/faulty_block_server.py --listen unix:/tmp/ambre-nbd.sock \
    --backing /tmp/ambre-disk.img --drop-unflushed-ratio 0.5 \
    --log /tmp/flushes.jsonl &
python3 host/powercut/powercut_campaign.py --image qnx80.img \
    --scenario host/scenarios/crash-50kmh.txt --nbd-socket /tmp/ambre-nbd.sock \
    --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 10 \
    --report /tmp/extreme.json
```

The orchestrator builds the QEMU line itself (`qemu_vm.QemuVM.command_line()`),
so step 1 is only a smoke test of the image. Options worth knowing:

* `--cache none --aio native` — ordered writes on the host side (defaults);
* `--blkdebug-config <file>` — `blkdebug:` error injection on the same drive
  (`-drive file=blkdebug:cfg:img,...`), for spec §7.6 fault injection;
* `--nbd-socket <path>` — drive served by `faulty_block_server.py`;
* `--list-cmd` / `--export-cmd` — guest commands used to collect the events
  (defaults `edrctl list --json`, then `edrctl export`);
* `--prompt` — serial console prompt marker (default `#`);
* `--boot-timeout` — bounded probe timeout, never a fixed sleep.

Outputs:

* `--report` — JSON: backend, sweep parameters, guest commands, summary
  (verdict counts, violations, cycle-time stats), one record per cut;
* `<report>.violations.jsonl` — one line per violation with the exact cut
  instant, the expected/observed event ids and the scenario path. These lines
  become the non-regression tests of spec §7.7: replay the same instant on the
  same image;
* `<report>.state.jsonl` — append-only state, what `--resume` reads.

Verdicts per cut: `ok`, `unexpected_loss` (an event that should have been
acquired is missing — a violation), `unexpected_presence` (an event present
although it could not have been acquired yet — a violation), `error`.

## Known limitation of this level (spec §7.4)

> Killing QEMU does not lose the writes already handed to the host system.
> This level tests the **ordering** and the **interruption** of the writes, not
> the loss of a volatile cache, which level 1 covers.

Concretely: after `SIGKILL`, the writes the guest already pushed to the host
block layer are in the host page cache or on the host disk, so they survive.
A guest that lies about durability (writes without flush, then reports
"acquired") cannot be caught by level 2 alone.

**How the faulty block server lifts it.** With `--nbd-socket`, the guest disk
is no longer a host file: it is served over NBD by `faulty_block_server.py`,
whose write-back cache lives in its own memory. Writes stay volatile until a
FLUSH order arrives; at the cut, each sector still unflushed is dropped with
probability `--drop-unflushed-ratio`. A guest that forgets to flush therefore
loses data on restart, which the campaign reports as `unexpected_loss`. The
same server journals every FLUSH order it receives, which is exactly the
measurement essay E1 needs.

Note the two settings that must agree: the drive uses `cache=writeback` on the
QEMU side (the cache under test is the server's, not QEMU's), and the drop
ratio is a *sector* probability — `1.0` is the worst case, `0.5` a realistic
half-written cache, `0.0` the level-2 control run.

The server has already been checked against the **real** QEMU block client:
`qemu-io -c "write ..." -c flush "nbd+unix:///?socket=/tmp/ambre-nbd.sock"`
connects, writes, sends the FLUSH and the server journals it. The remaining
unknown is only the QNX guest behaviour (does the QNX block driver emit a
FLUSH for a raw partition — essay E1).

## What is still missing until the QNX image exists

1. `e1_flush_probe.py --mode guest`: run it on the image and confirm that
   `sync` produces an `NBD_CMD_FLUSH` for a **raw** device. If it does not, use
   the fallback of essay E1 (dedicated Power-Safe partition, `fsync()` via
   `edrctl fsync-test`, longer freeze window) — the command constants are
   already in the file, marked `TODO(qnx-sdp)`.
2. Serial console: confirm the prompt marker (`--prompt`) and that the shell
   runs on `-serial`. The `SerialConsole` transport is written but has never
   met a real guest.
3. `edrctl list --json` / `edrctl export`: the output shapes the campaign
   parses are tolerant, but the real field names must be checked
   (`parse_events` accepts a bare list, `{"events": [...]}` and a single
   object).
4. Boot time: E3 measures it; if a cycle exceeds 10 s, shrink the boot image or
   raise `--jobs` (the spec's fallback for E3).
5. `--blkdebug-config`: write the injection rules for §7.6 once the write
   pattern of the EDR is known (which QEMU event to fail: `write_aio`,
   `flush_to_os`, ...).
6. Scenario files: `host/scenarios/*.txt` are produced by another agent; the
   campaign only needs `inject` / `acquire` / `duration_ms` lines.

## Tests

`tests/test_powercut_py.py` (unittest, stdlib, ~7 s):

* VM lifecycle in dry-run: start, probe, kill, restart, cycle, cleanup;
* a 5-cut campaign with a valid JSON report, `--resume` replaying nothing;
* violations detected when a "not yet durable" window is simulated;
* E3 over 5 cycles with min/median/p99/max;
* faulty block server: write retained then dropped at the cut, flushed write
  kept, exact FLUSH journalling, 50 % drop ratio over 200 writes, ratio 0;
* E1 host mode: the FLUSH is received, journalled and the data is durable.

No orphan process is left behind: every VM is stopped in a `finally` block and
`qemu_vm` also stops any live machine at interpreter exit.
