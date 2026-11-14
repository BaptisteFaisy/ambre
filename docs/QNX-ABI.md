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

Owned by Ambre, and now **encoded the QNX way**: the numbers live in
`target/edr_dcmd.h`, the single source used by the service
(`target/journal/edr_journal_io_qnx.c`) and by its clients
(`host/qnx_probe/devctl_probe.c`). On target the codes are built with QNX's
`__DIOF()` / `__DIOTF()` / `__DION()` macros from `<devctl.h>`, class
`_DCMD_MISC` (the class QNX recommends for a devctl that is not a QNX driver).

**Why the raw project numbers could not work.** A devctl code carries a
*direction* in its two top bits and a *size* in bits 16-31
(`get_device_size()` = `((cmd & ~DEVDIR_TOFROM) >> 16)`, devctl.h). The former
project constants (`0x45000001`…) encoded a size of **0** and a class outside
QNX's table (`0x000`–`0x12ff`), so the kernel copied **nothing** back to the
client: `devctl()` returned EOK with the client's buffer untouched. Measured
on target, 2026-11-14:

```
devctl DCMD_EDR_STATUS: rc=25 errno=0 (No error) code=1281 size=68
```

The repository header had anticipated this without being able to do it
("re-encode with QNX's `__DIOTF()` macros on target") — that is what
`target/edr_dcmd.h` does now.

**How a devctl answer is carried.** The reply of an `_IO_DEVCTL` is a
`_io_devctl_reply` (sys/iomsg.h:590-596): a 16-byte header followed by the
payload. Two forms were measured on target and both are wrong:

| Form | What the client got |
|---|---|
| `_RESMGR_PTR(ctp, &body, sizeof body)` | the body **shifted by 16 bytes** — the kernel read the first 16 bytes of the body as the header (512 landing in `open_events`, 1 in `sealed_events`) |
| `return EOK` with no `_RESMGR_PTR()` | **nothing** — the reply carried the header alone, the client buffer stayed as it was |

The form that works: write the body in the message payload
(`_IO_OUTPUT_PAYLOAD(msg)`), set `msg->o.nbytes`, and reply with the message
itself — `_RESMGR_PTR(ctp, msg, sizeof(io_devctl_t) + msg->o.nbytes)`. The
service does exactly that, and the client now receives what the service sent:
`devctl_probe status` renders `mode=1 health=0 backend=2`, `bytes_written=512`,
`flushes=1` — the very values `edrctl status` renders over the IPC transport
on the same journal.

**Bodies.** The replies are the frozen IPC bodies
(`target/edr_ipc.h`, `target/journal/edr_journal_svc.h`), not new structures:
a status is `ambre_ipc_status_body_t`, a list is `ambre_edr_list_body_t`
(count + lines), an export is `ambre_edr_export_body_t` followed by the raw
blocks. Each one is **self-describing** — `devctl()`'s `dev_info_ptr` is
defined *by the driver*, not by the kernel (devctl.html: "the associated
dev_info_ptr values … depend on the device driver"), so a client cannot read
a length there and cannot infer one.

| Command | Encoding | Payload direction | Body | Requirement |
|---|---|---|---|---|
| `DCMD_EDR_STATUS` | `__DIOF(_DCMD_MISC, 1, …)` | server -> client | `ambre_ipc_status_body_t` | OBS-02 |
| `DCMD_EDR_LIST` | `__DIOF(_DCMD_MISC, 2, …)` | server -> client | `ambre_edr_list_body_t` | EXP-01 |
| `DCMD_EDR_EXPORT` | `__DIOTF(_DCMD_MISC, 3, uint32_t[2])` | `uint32_t[2]` (event_id, first_block) -> raw blocks | `ambre_edr_export_body_t` + up to 7 × 512 B | EXPORT |
| `DCMD_EDR_AUTOTEST` | `__DION(_DCMD_MISC, 4)` | none -> `EOK` / `EIO` | — | OBS-04 |

**Access control.** The kernel does not police these devctls; the service
does, through the same peer-type matrix as the IPC transport
(`edr_gate()`, `target/journal/edr_journal_io_qnx.c`): the *service* peer
(gid 4) is served, a *producer* (gid 3) is refused with `EACCES`. Measured on
target, 2026-11-14:

```
on -u 0:4 /proc/boot/devctl_probe status   -> STATUS mode=1 health=0 backend=2 …
on -u 0:3 /proc/boot/devctl_probe status   -> devctl DCMD_EDR_STATUS: rc=13 (EACCES)
```

**Still open.** The per-event nodes by path (`/dev/edr/events/<id>`, EXP-08's
`read()` form) are not served: the OCB is now the one the QNX library
allocates, and it carries no event id. The raw blocks of an event are
therefore read through `DCMD_EDR_EXPORT`, which is measured.

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
