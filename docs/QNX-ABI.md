# QNX 8.0 ABI Reference for Ambre

This document lists every numeric value, constant, structure size and flag that
the Ambre QNX glue depends on, where its value comes from in the QNX SDP 8.0
documentation, and how it is verified.

> **Honesty boundary (`docs/QNX-PORT.md`)**: compiling the glue on the PC
> against `target/qnx-stub/include/` (`make qnx-syntax`) proves **only** that
> the prototypes, field names and C types match the declarations we wrote in the
> stubs. It proves **nothing** about the numeric values below or about runtime
> kernel behaviour. Every numeric dependency is therefore guarded by
> `_Static_assert` in `target/tests/qnx_abi_assert.c`, which runs **only**
> against the real SDP 8.0 headers when `qcc` compiles on target (`#ifdef __QNX__`).

---

## 1. Values the glue depends on numerically

These constants are compared against kernel-provided fields or indexed with.
If the SDP value differs from the number assumed here, the QNX glue will
misbehave at runtime; `target/tests/qnx_abi_assert.c` refuses to compile on
QNX if any of them diverges.

| Symbol | Header | Expected value | Used where | QNX 8.0 doc provenance |
|---|---|---|---|---|
| `_PULSE_CODE_DISCONNECT` | `<sys/neutrino.h>` | `-1` (symbolic on PC, verified on target) | `ambre_port_ipc_qnx.c`, `edr_capture_qnx.c` (QNX-05) | C Library Reference, `MsgReceive()` / System Pulses |
| `_PULSE_CODE_UNBLOCK` | `<sys/neutrino.h>` | `-2` | `ambre_port_ipc_qnx.c` | C Library Reference, `MsgReceive()` / System Pulses |
| `_PULSE_CODE_MINAVAIL` | `<sys/neutrino.h>` | `0` | `edr_capture_qnx.c` (`EDR_CAP_TICK_CODE`) | C Library Reference, `MsgSendPulse()` / System Pulses |
| `_PULSE_CODE_MAXAVAIL` | `<sys/neutrino.h>` | `127` | bounds check on pulse codes | C Library Reference, `MsgSendPulse()` |
| `_NTO_TRACE_USERFIRST` | `<sys/trace.h>` | `0x2000` (range check `<= USERLAST`) | `ambre_port_qnx.c` (`ambre_port_trace`, OBS-03) | C Library Reference, `TraceEvent()` |
| `_NTO_TRACE_USERLAST` | `<sys/trace.h>` | `0x2fff` | `ambre_port_qnx.c` (5 events `USERFIRST + 1..5`) | C Library Reference, `TraceEvent()` |
| `EOK` | `<errno.h>` on QNX | `0` | all `*_qnx.c` files | C Library Reference, `<errno.h>` / `EOK` |
| `SCHED_FIFO` | `<sched.h>` | POSIX standard (`> 0`) | `ambre_port_qnx.c` (`SchedSet`) | C Library Reference, `SchedSet()` |

---

## 2. Values passed symbolically (never compared numerically)

The glue only passes these constants into QNX kernel/resmgr calls or masks; it
never decodes or compares their bit patterns. Their stub values are therefore
**placeholders** on the PC, and the QNX build substitutes the SDP's own header
values automatically.

| Symbol | Header | Role in Ambre | Provenance |
|---|---|---|---|
| `DCMD_FLUSH_CACHE` / `DCMD_BLK_FLUSHCACHE` | `<sys/dcmd_blk.h>` | `devctl(fd, DCMD_FLUSH_CACHE, ...)` to io-blk (STO-01, essay E1) | Utilities Reference, `io-blk`; C Library Reference, `devctl()` |
| `_NTO_CHF_UNBLOCK`, `_NTO_CHF_DISCONNECT`, `_NTO_CHF_PRIVATE` | `<sys/neutrino.h>` | `ChannelCreate()` flags (QNX-03, QNX-05) | C Library Reference, `ChannelCreate()` |
| `_NTO_SIDE_CHANNEL` | `<sys/neutrino.h>` | `ConnectAttach()` for self-pulsing timer (QNX-03) | C Library Reference, `ConnectAttach()` |
| `SIGEV_PULSE` | `<sys/neutrino.h>` | `struct sigevent.sigev_notify` for timer pulses | C Library Reference, `sigevent` |
| `_RESMGR_FLAG_DIR`, `_RESMGR_DETACH_PATHNAME`, `_FTYPE_ANY` | `<sys/resmgr.h>`, `<sys/ftype.h>` | `resmgr_attach()` / `resmgr_detach()` on `/dev/edr` (QNX-01) | C Library Reference, `resmgr_attach()` |
| `POOL_FLAG_EXIT_SELF` | `<sys/dispatch.h>` | `thread_pool_create()` (QNX-05) | C Library Reference, `thread_pool_create()` |
| `SLOG2_ERROR`, `SLOG2_WARNING`, `SLOG2_INFO`, `SLOG2_DEBUG1`, `SLOG2_DEBUG2`, `SLOG2_NO_FLAGS` | `<sys/slog2.h>` | `slog2_register()` / `slog2f()` (OBS-01) | C Library Reference, `slog2_register()`, `slog2f()` |
| `CLOCK_MONOTONIC` | `<time.h>` | `ClockTime()` / `timer_create()` (ACQ-02, VIE-02) | C Library Reference, `ClockTime()` |

---

## 3. Application-defined devctls on `/dev/edr`

Owned by Ambre: the numbers are **invented by the project**, defined in
`target/journal/edr_journal_qnx.c` (deliberately NOT in the stub headers, which
only mirror real QNX declarations) and documented here. They are project
constants, not kernel ABI, so `target/tests/qnx_abi_assert.c` does not assert
them; both ends of the wire are compiled from the same source. On target they
should eventually be re-encoded with QNX's `__DIOTF()` class macros from
`<devctl.h>`; until an SDP is available the raw numbers below are the contract
between `edr-journal` and its clients (`edrctl`, `edr-archive`).

| Command | Value | Payload direction | Body | Requirement |
|---|---|---|---|---|
| `DCMD_EDR_STATUS` | `0x45000001` | server -> client | `ambre_ipc_status_body_t` | OBS-02 |
| `DCMD_EDR_LIST` | `0x45000002` | server -> client | `ambre_ipc_event_line_t[]` | EXP-01 |
| `DCMD_EDR_EXPORT` | `0x45000003` | `uint32_t[2]` (event_id, first_block) -> raw blocks | up to 7 x 512 B | EXPORT |
| `DCMD_EDR_AUTOTEST` | `0x45000004` | none -> `EOK` / `EIO` | — | OBS-04 |

---

## 4. Structure sizes and wire contracts

Verified on **both** PC and QNX (independent of stub headers):

| Type | Size (bytes) | Header |
|---|---|---|
| `ambre_ipc_hdr_t` | `16` | `target/edr_ipc.h` |
| `ambre_block_t` | `512` | `fmt/ambre_types.h` |
| `ambre_alloc_table_t` | `512` | `fmt/ambre_types.h` |
| `ambre_ipc_status_body_t` | `68` (packed) | `target/edr_ipc.h` |
| `ambre_ipc_event_line_t` | `32` (packed) | `target/edr_ipc.h` |

In addition, `target/journal/edr_journal_qnx.c` relies on `iofunc_ocb_t` being
the first member of `struct edr_qnx_ocb` (`offsetof(struct edr_qnx_ocb, ocb) == 0`),
which is checked unconditionally on both PC and QNX.

---

## 5. Security abilities & policy alignment (SEC-01, QNX-06)

`target/journal/edr_journal_qnx.c` maps `info.cred.egid` values `1..5` to
`AMBRE_PEER_JOURNAL..AMBRE_PEER_ARCHIVE`. Those group IDs **must** match the
numeric GIDs assigned in the boot image / `image/ambre.sec` (owned by the
`image-sec` agent). Compiling against stubs cannot verify that mapping.
