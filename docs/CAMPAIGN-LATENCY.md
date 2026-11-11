# Latency campaign — what was measured on the PC, and what was not

Status: 2026-11-11. Backend `posix` (target/port/ambre_port_posix*.c). Every
number below comes from a run that was actually executed on the machine named
in section 2, with the command quoted next to it. Nothing is extrapolated, and
no QNX number is presented as measured: the requirements that need the target
are listed as **not measured** in section 7.

Specification: `docs/cahier-des-charges.md` §5.7 (PERF-01..PERF-08), §7.5 (the
three loads), §1.4 (jury criteria), §11 (the "QEMU durations are contested"
risk → separate the software share from the support share).

## 1. What is measured, and how

The three processes of the chain are the real ones, started as separate OS
processes: `edr-journal` (with `AMBRE_TRACE=<file>`), `edr-capture`,
`edr-replay`. No component is stubbed, no timing is injected.

| Requirement | Measurement | Where it comes from |
|---|---|---|
| PERF-01 | last sample crossing the threshold → trigger pulse | `TRIGGER` (1) trace timestamp of the journal **minus** `emerg.trig_ts_us`, the capture's own stamp of the crossing sample, read back from the journal image with `host/mcap/ambre_journal_reader.py` |
| PERF-02 | trigger → "acquis", total freeze | `TRIGGER` (1) → `FLUSH_ACK` (3) in the trace |
| PERF-03 | **software share** of that freeze | the same `TRIGGER` → `FLUSH_ACK`, measured with the journal file on **tmpfs** (`/dev/shm`), where `fdatasync()` has no device to talk to |
| PERF-05 | cost of sending one frame, producer side | `edr-replay`'s own round-trip histogram (`ambre_port_conn_send(..., &latency_ns)`, PERF-05 / essay E4) |

The freeze is decomposed explicitly, and the report says which part is
measured and which part is derived:

```
dispatch_us       = WRITE_SUBMIT - TRIGGER          (message handling only)
write_flush_us    = FLUSH_ACK    - WRITE_SUBMIT     (encode + CRC + table commit + flush)
freeze_us         = FLUSH_ACK    - TRIGGER          (PERF-02)
software pass     = the same freeze, journal on tmpfs  (PERF-03, measured)
media (derived)   = freeze on NVMe - freeze on tmpfs   (a subtraction, not a measurement of the device)
```

### Two structural facts the harness had to work around

Both are properties of the code under measurement, reported rather than hidden:

1. **One crash per journal boot.** The live journal holds 8 event slots
   (`AMBRE_MAX_EVENTS`) and `ambre_journal_register_event()` refuses the 9th
   with `AMBRE_ERR_FULL`. `ambre_table_pick_victim()` (the overwrite policy,
   STO-06) exists in `fmt/ambre_fmt_table.c` and is unit tested, but the
   journal engine never calls it. The campaign therefore splits the scenario
   into one chunk per shock and replays each chunk on a fresh journal boot;
   the number of boots is reported (`runs`) and so is every refused trigger.
2. **One crash yields a burst of trigger events.** The capture runs the
   trigger engine in `AMBRE_TRIG_WINDOW` mode, so while the 150 ms window
   still holds the crash delta-v the engine fires again after each event end.
   Each of those is a full trigger → acquired cycle and is a legitimate
   measurement of PERF-02. They are *not* a legitimate measurement of PERF-01:
   during a burst the capture blocks on the journal's answer, so its
   interpolated sample timeline drifts behind the wall clock. The report
   therefore separates **crash triggers** (event id 1 of a boot) from
   **re-fires**, and never merges the two for PERF-01.

### Other honesty notes baked into the harness

- A virgin partition is formatted at start-up (48 MiB of zeros). The harness
  drains that with one `fdatasync()` before the first trigger, so no freeze is
  paying for the format.
- `edrctl status --json` / `list --json` answer *"the portability layer has no
  client-side reply"* on this backend: `target/ctl/edr_ctl.c` compiles its
  request/response seam only when `AMBRE_PORT_HAS_CONN_REQUEST` is defined and
  `target/port/ambre_port.h` does not declare `ambre_port_conn_request()`.
  The journal counters are therefore read from the journal's own status line,
  which it logs at start-up, on `SIGUSR1` (OBS-02) and at exit; the per-event
  `trig_ts_us` comes from the journal image. This is a limitation of the
  service path, not of the freeze path.
- Percentiles use the **nearest-rank** convention
  (`sorted(v)[ceil(p/100*n)-1]`), the same one as `edr-replay`'s histogram.
  PERF-05 values are the **bucket ceilings** `edr-replay` reports (10, 25, 50,
  100, 250, 500 µs, 1, 5 ms), so they are upper bounds, not exact quantiles.

## 2. The machine

| | |
|---|---|
| CPU | 11th Gen Intel(R) Core(TM) i5-11600K @ 3.90GHz, 12 logical cores |
| RAM | 31 GiB (33 546 559 488 bytes) |
| Kernel | 6.14.0-37-generic |
| Filesystem | ext4 on NVMe (`/dev/nvme0n1p2`), tmpfs on `/dev/shm` for the software pass |
| Compiler | gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Python | 3.12.3 |
| Backend | `posix` — a regular file as the raw partition, `CLOCK_MONOTONIC` in µs, AF_UNIX `SOCK_SEQPACKET`, `fdatasync()` as the flush |

## 3. Protocol

```sh
# one multi-shock scenario: 30 shocks, three delta-v amplitudes, 1 s apart
python3 host/latency/multi_shock.py --triggers 30 --cycle-ms 1000 \
    --dv-kmh 9.0,10.8,12.6 --out build-lat/scenario-latency.txt

# one campaign per load: 200 measured events, media pass + software pass + PERF-05 run
python3 host/latency/measure.py --bins build-lat \
    --scenario build-lat/scenario-latency.txt --triggers 200 \
    --load <none|cpu|disk|mem> --report build-lat/latency-<load>.json
```

`make latency` runs the `cpu` campaign (`LOAD=` overrides), `make latency-all`
runs the four loads of §7.5. The scenario builder is
`host/latency/multi_shock.py`: flat deceleration pulses at −250 000 mm/s²
(≈ 25.5 g, every sample a multiple of 1000 mm/s² so the integer rectangle rule
of the trigger engine integrates them exactly), 10 / 12 / 14 ms long for the
three requested delta-v values 9.0 / 10.8 / 12.6 km/h — all above the 8 km/h
trigger threshold and far below the 25 km/h lock threshold, so no event is ever
locked. The 30 shocks cycle through those three amplitudes.

The scenario defines **one shock per journal boot**; the number of boots is a
consequence of fact 1 in section 1, not a protocol choice. `--triggers 200`
means 200 measured trigger→acquired cycles, whatever the number of boots.

### The three loads of §7.5 (`host/latency/load_gen.py`)

Each load is real OS processes, all at nice 19 (less priority than the
recorder, which is what §7.5 (a) asks for), each with its own deadline so no
orphan can survive the campaign; the harness additionally `terminate()`s then
kills them and waits.

| Load | What it does |
|---|---|
| `cpu` | 12 processes (= one per logical core) spinning on integer arithmetic for the duration of the campaign — CPU saturated by lower-priority tasks |
| `disk` | 1 process writing 4 MiB files and `fdatasync()`ing each of them in the campaign's archive directory, so the device and the flush path are busy while the recorder writes its journal |
| `mem` | 1 process holding 35 % of `MemAvailable` (floored so the machine never drops below 2 GiB) in anonymous memory and re-touching one page every 20 ms, so the page cache is under reclaim — 5974 MiB held in the published run |
| `none` | baseline, no load |

## 4. Results

All four campaigns: 2026-11-11, reports stamped 17:18–17:23 UTC, 200 measured
events each, on the machine of section 2. Values in microseconds.

### 4.1 Summary

| Load | PERF-01 crash triggers (n) median / p99 / max | PERF-02 freeze median / p99 / p99.9 / max | PERF-03 software median / p99 / p99.9 / max | Boots | Refused |
|---|---|---|---|---|---|
| none | 1082 / 7329 / 7329 (25) | 3479 / 10 922 / 16 931 / 16 931 | 10 / 58 / 80 / 80 | 26 | 1245 |
| cpu | 1089 / 9130 / 9130 (26) | 4308 / 15 251 / 44 275 / 44 275 | 10 / 110 / 918 / 918 | 27 | 1193 |
| disk | 1076 / 2077 / 2077 (25) | 3437 / 10 667 / 23 490 / 23 490 | 10 / 237 / 4045 / 4045 | 26 | 1250 |
| mem | 1074 / 10 324 / 10 324 (25) | 3424 / 9749 / 21 222 / 21 222 | 10 / 56 / 84 / 84 | 26 | 1348 |

Overshoots against the §5.7 limits, counted over the measured events:
PERF-01 (> 2000 µs) 4 / 8 / 1 / 4 of 25–26 crash triggers; PERF-02 (> 20 ms)
0 / 1 / 1 / 1 of 200; PERF-03 (> 500 µs) 0 / 1 / 1 / 0 of 200.

"Refused" counts triggers the journal answered with `AMBRE_ERR_FULL`: the
8-slot table was full because the burst of re-fires of the same crash had
already consumed every slot. They are refused *after* the crash event was
acquired, so no measurement is lost, but the counter is published rather than
hidden. Boots includes the retried ones: a chunk that produced no event is
retried and recorded in `failures` (1 retry in the `none`, `cpu`, `disk` and
`mem` media passes, 3 in the `mem` software pass).

### 4.2 Decomposition: software share vs support share

Median, from the two passes of each campaign:

| Load | software (measured, tmpfs) | media (derived) | total (NVMe) | media share |
|---|---|---|---|---|
| none | 10 µs | 3469 µs | 3479 µs | 99.7 % |
| cpu | 10 µs | 4298 µs | 4308 µs | 99.8 % |
| disk | 10 µs | 3427 µs | 3437 µs | 99.7 % |
| mem | 10 µs | 3414 µs | 3424 µs | 99.7 % |

p99:

| Load | software p99 | media p99 (derived) | total p99 |
|---|---|---|---|
| none | 58 µs | 10 864 µs | 10 922 µs |
| cpu | 110 µs | 15 141 µs | 15 251 µs |
| disk | 237 µs | 10 430 µs | 10 667 µs |
| mem | 56 µs | 9693 µs | 9749 µs |

The software share of the freeze is 10 µs at the median on every load and stays
under 240 µs at p99. The whole remaining freeze is the `fdatasync()` of a file
on NVMe. That is exactly the split §11 asks for, and it is the reason a PC
freeze number cannot be read as a target number.

### 4.3 Histograms

Freeze (PERF-02), media pass, 200 events per campaign (buckets below 2.5 ms are
empty everywhere and omitted here; the harness prints them):

```
none                     cpu                      disk                     mem
  <=   5000 us | 149 74.5%  <=   2500 us |   1  0.5%  <=   2500 us |  50 25.0%  <=   2500 us |  28 14.0%
  <=  10000 us |  48 24.0%  <=   5000 us | 127 63.5%  <=   5000 us |  96 48.0%  <=   5000 us | 133 66.5%
  <=  25000 us |   3  1.5%  <=  10000 us |  62 31.0%  <=  10000 us |  50 25.0%  <=  10000 us |  37 18.5%
  <=  50000 us |   0  0.0%  <=  25000 us |   9  4.5%  <=  25000 us |   4  2.0%  <=  25000 us |   2  1.0%
  <= 100000 us |   0  0.0%  <=  50000 us |   1  0.5%  <=  50000 us |   0  0.0%  <=  50000 us |   0  0.0%
  <= 250000 us |   0  0.0%  <= 100000 us |   0  0.0%  <= 100000 us |   0  0.0%  <= 100000 us |   0  0.0%
```

PERF-01, crash triggers only (re-fires excluded, see section 1):

```
none (25)                cpu (26)                 disk (25)                mem (25)
  <=   100 us |  1  4.0%   <=  2500 us | 19 73.1%   <=  2500 us | 25 100 %   <=   100 us |  1  4.0%
  <=  2500 us | 22 88.0%   <=  5000 us |  4 15.4%                            <=   500 us |  1  4.0%
  <=  5000 us |  1  4.0%   <= 10000 us |  3 11.5%                            <=  2500 us | 19 76.0%
  <= 10000 us |  1  4.0%                                                     <=  5000 us |  2  8.0%
                                                                             <= 10000 us |  1  4.0%
                                                                             <= 25000 us |  1  4.0%
```

### 4.4 The three worst cases, line by line (PERF-07 on the PC trace)

From `build-lat/latency-disk.json` (the campaign with the worst freeze). Each
line is a decomposition of the `AMBRE_TRACE` timestamps of that event; the
"media (derived)" figure subtracts the median `WRITE_SUBMIT → FLUSH_ACK` of the
tmpfs pass, which is why it is labelled derived and not measured.

```
run 11 event 2 (re-fire)   freeze 23 490 us
  TRIGGER      110 997 420 106
  WRITE_SUBMIT 110 997 420 110   +4 us        dispatch: message handling
  FLUSH_ACK    110 997 443 596   +23 486 us   write + CRC + table commit + fdatasync (NVMe)
  SEAL         110 997 453 016   +9 420 us    after the event is acquired, not part of the freeze
  PERF-01 on the same event: 9 091 us
  media (derived) 23 477 us -> 99.9 % of the freeze is the support

run 1  event 4 (re-fire)   freeze 12 996 us
  TRIGGER      110 975 020 209
  WRITE_SUBMIT 110 975 020 211   +2 us
  FLUSH_ACK    110 975 033 205   +12 994 us   fdatasync queued behind the load generator's flushes
  SEAL         110 975 044 962   +11 757 us
  PERF-01 on the same event: 46 171 us
  media (derived) 12 985 us

run 5  event 6 (re-fire)   freeze 10 667 us
  TRIGGER      110 979 858 356
  WRITE_SUBMIT 110 979 858 361   +5 us
  FLUSH_ACK    110 979 869 023   +10 662 us
  SEAL         110 979 884 208   +15 185 us
  PERF-01 on the same event: 9 094 us
  media (derived) 10 653 us
```

The shape is the same in all four campaigns: the dispatch step is 1 to 3 µs at
the median and 46 to 103 µs at p99, and essentially the whole freeze is in
`WRITE_SUBMIT → FLUSH_ACK`, i.e. in the device flush. The worst case of the
`none` campaign (16 931 µs) is a background flush of the host filesystem landing
on the journal's flush; the worst case of `cpu` (44 275 µs) is the same effect
under scheduler contention. The software path stays under 500 µs everywhere
except two events (918 µs under CPU load, 4045 µs under disk load — the two
PERF-03 overshoots of §6).

### 4.5 PERF-05 — and the honest negative

`edr-replay` sends a frame and waits for the journal's acknowledgement; that
round trip is the PERF-05 measurement. The reference run is a shock-free drive
(`host/scenarios/no-crash.txt`, 20 000 rows at 1 kHz, 2000 frames) so no
trigger blocks the path. All four reference runs completed on the first
attempt; the values are `edr-replay`'s own bucket ceilings:

| Load | frames | p50 | p99 | p99.9 | worst | mean | ≤ 50 µs | GAP records |
|---|---|---|---|---|---|---|---|---|
| none | 2000 | ≤ 50 µs | ≤ 1 ms | ≤ 5 ms | 7.10 ms | 78 µs | 1672 = 83.6 % | 34 |
| cpu | 2000 | ≤ 50 µs | ≤ 1 ms | ≤ 5 ms | 4.74 ms | 65 µs | 1750 = 87.5 % | 102 |
| disk | 2000 | ≤ 50 µs | ≤ 5 ms | ≤ 5 ms | 26.6 ms | 122 µs | 1426 = 71.3 % | 64 |
| mem | 2000 | ≤ 50 µs | ≤ 5 ms | ≤ 5 ms | 8.49 ms | 114 µs | 1410 = 70.5 % | 78 |

**PERF-05 is not met on this backend**, and the report says so
(`requirements["PERF-05"]`, and `verdict: p99 EXCEEDS the 50 us requirement` in
`edr-replay`'s own output): the p99 ceiling is 1 ms under no load and 5 ms
under disk and memory pressure, against a 50 µs requirement. Two causes, both
measured:

1. The measured cost is a **round trip that waits for the acknowledgement**, not
   the producer's own bookkeeping: the producer blocks until `edr-capture` has
   answered, and the capture answers only after having forwarded to the journal
   whatever the frame implies.
2. The quiet drive is not empty: the journal image of that run holds 34 to 102
   `GAP` records. The 1 kHz scenario replayed at rate 1.0 on a host that stalls
   closes a signal gap, and each gap costs one synchronous journal round trip
   (record write) on the input path. Those frames are the slow ones.

An earlier execution of the same protocol (16:43–16:48 UTC) gave p99 ceilings of
5 ms under no load and CPU load and a `mem` reference run that failed twice
(rc = 1, 0 frames: the producer's fixed 500 ms send timeout was reached while
the page cache was being reclaimed). That run was replayed by hand under the
same load — 200 frames, mean 211 µs, p50 ≤ 50 µs, p99 ≤ 5 ms, worst 10.1 ms —
and is reported here as a 200-frame run, not as the 2000-frame one. Only the
17:18–17:23 UTC batch is kept as artifacts.

## 5. What the software share does and does not show

- The **software path** of a freeze — message handling, one contiguous EMERG
  write, the allocation-table commit, and the flush call — costs **7 to
  4045 µs** on this machine (median 10 µs on every load, p99 56–237 µs, worst
  value 4045 µs under disk load). That is the part the code owns, and it is the
  part PERF-03 targets (500 µs).
- The **support share** dominates everything else: 99.7 to 99.8 % of the
  measured freeze. On this backend the support is a page-cache write plus an
  NVMe `fdatasync()`, whose cost depends on what else is flushing.
- Under `disk` load the *software* share degrades the most (p99 237 µs, one
  event at 4045 µs) and under `cpu` load one event reaches 918 µs: the archive
  writer's `fdatasync()` calls and the spinners compete with the journal's own
  flush, and the table commit reads the older copy back before writing the
  newer one. Those are the only two events out of 800 that exceed the 500 µs
  budget of PERF-03. This is the one place where the §11 risk "an archive write
  delays the freeze" is visible in a measurement.

## 6. Requirements status

| ID | Target | Measured here | Verdict |
|---|---|---|---|
| PERF-01 | ≤ 2 ms | median 1074–1089 µs, p99 2077–10 324 µs, max 10 324 µs (crash triggers of a fresh journal) | **not met at p99**: the median is inside the limit, but 1 to 8 crash triggers out of 25–26 exceed it, the worst being a capture descheduled behind a busy device |
| PERF-02 | ≤ 20 ms on reference HW | median 3424–4308 µs, p99 9749–15 251 µs, p99.9 16 931–44 275 µs, max 44 275 µs; 3 events out of 800 over 20 ms (one under `cpu`, one under `disk`, one under `mem`) | inside the limit on this PC, but 99.7 % of it is an NVMe `fdatasync()`: **not a statement about the reference hardware** |
| PERF-03 | ≤ 500 µs | median 10 µs, p99 56–237 µs, p99.9 80–4045 µs, max 4045 µs | **met** at the median and at p99 under every load; 2 events out of 800 exceed the budget (one under `cpu`, one under `disk`) |
| PERF-04 | 0 overshoot over 1000 triggers (S), 10 000 under load (C) | 200 triggers per campaign, 800 in total | volume not reached, and the overshoots above are not zero: **not met as stated** |
| PERF-05 | < 50 µs at p99 | p99 ceiling 1 ms (none, cpu), 5 ms (disk, mem) | **not met** — see 4.5 |
| PERF-06 | capture < 1 s after kernel start | — | not measured (needs the target) |
| PERF-07 | three worst cases explained by a **kernel** trace | worst cases decomposed from the `AMBRE_TRACE` log file | partially: the decomposition exists, the kernel trace does not |
| PERF-08 | same measurements on a Raspberry Pi 5 | — | not measured |

## 7. What these numbers are not

- **They are not QNX numbers.** The backend is `posix`: the partition is a
  regular file, the flush is `fdatasync()`, the transport is AF_UNIX
  `SOCK_SEQPACKET`, the trace is a text file written by
  `ambre_port_trace()`. The QNX backend would use `/dev/hd0tXX` through io-blk
  with `DCMD_FLUSH_CACHE`, `MsgSend`/pulses, `TraceUser` events and
  `SCHED_FIFO` priorities. Nothing in this campaign exercises any of that.
- **The freeze number is not representative of the target.** On this host 99.7 %
  of the freeze is an NVMe `fdatasync()` of a file; on the target it would be
  an eMMC flush through the QNX cache machinery. Only the software share
  (section 5) transfers, and even that only as an order of magnitude: the
  target's CPU and clock are different.
- **PERF-02, PERF-06, PERF-07 and PERF-08 remain "not measured on target"**
  (docs/QNX-PORT.md). PERF-07's requirement is a *kernel* trace; what is
  published here is a decomposition from the portability layer's own trace
  points, which is the same five events but not a kernel trace.
- **No instruction-count mode.** The §7.5/§11 defence against "QEMU durations
  are contested" is reproducible instruction counting under QEMU. This campaign
  is wall-clock on bare metal, so it is reproducible only in distribution, not
  event by event. The same campaign was executed three times on 2026-11-11; the
  published artifacts are the last execution (17:18–17:23 UTC). Comparing it
  with the previous one (16:43–16:48 UTC, whose artifacts the last execution
  overwrote), the freeze medians moved by 10 µs under no load (3489 → 3479),
  0.5 ms under disk load (3972 → 3437), 0.2 ms under memory pressure
  (3248 → 3424) and **1.6 ms under CPU load (2727 → 4308)**. The software share
  is stable (median 10 µs in every execution of every load); the media share is
  not, which is exactly why the two are published separately.
- **PERF-05's number is a round trip, not a send cost.** It includes the
  journal's acknowledgement and, on gap frames, a synchronous record write.
  Published as bucket ceilings, it is an upper bound on the cost, which makes
  the "not met" verdict safe: the true value cannot be larger than the ceiling.
- **The 8-slot limit is a property of the code, not of the campaign.** The
  measurement needs one journal boot per crash because the live engine does not
  recycle table slots. Fixing that (`ambre_table_pick_victim()` is written and
  tested but not wired into `ambre_journal_register_event()`) would let one
  boot carry a long campaign; it would also change the freeze path, so it is
  reported here rather than done in passing.

## 8. Reproducing

```sh
make BUILD=build-lat processes          # edr-journal, edr-capture, edr-replay
make BUILD=build-lat latency            # 200 events under CPU load
make BUILD=build-lat latency-all        # none, cpu, disk, mem
python3 tests/test_latency_py.py        # harness tests, < 90 s
```

Provenance of the numbers of section 4 (all four runs, same code, same
machine, reports stamped 17:18–17:23 UTC):

| Load | command | machine report | console output |
|---|---|---|---|
| none | `python3 host/latency/measure.py --bins build-lat --scenario build-lat/scenario-latency.txt --triggers 200 --load none --report build-lat/latency-none.json` | `build-lat/latency-none.json` | `build-lat/campaign-none.log` |
| cpu | same, `--load cpu --report build-lat/latency-cpu.json` | `build-lat/latency-cpu.json` | `build-lat/campaign-cpu.log` |
| disk | same, `--load disk --report build-lat/latency-disk.json` | `build-lat/latency-disk.json` | `build-lat/campaign-disk.log` |
| mem | same, `--load mem --report build-lat/latency-mem.json` | `build-lat/latency-mem.json` | `build-lat/campaign-mem.log` |

The scenario file is `build-lat/scenario-latency.txt` (30 800 rows, 30 shocks,
built by the command of section 3). Every event of both passes of every
campaign is in the JSON under `events.media` / `events.software`.

The JSON reports carry `backend`, `host`, `kernel`, `compiler`, every event of
both passes, the decomposition, the three worst cases with their trace
timestamps, and `not_measurable_without_qnx: [...]`.
