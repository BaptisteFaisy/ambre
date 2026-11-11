# Ambre — Security, Privacy, and Supervision Architecture

This document defines the trust chain, process isolation, access-control matrix,
privacy guarantees, and process supervision model for the Ambre Event Data
Recorder across both the PC verification backend and the QNX 8.0 target.

> **Verification boundary**: Portable access-control logic
> (`ambre_port_access_allowed()` in `target/port/ambre_port_posix.c`) and static
> image/policy consistency (`scripts/check-image.sh`) are **verified on PC**.
> Kernel-enforced QNX security policy (`image/ambre.sec`), `secpolcompile`,
> `secpolpush`, and native QNX High Availability Manager (HAM) supervision are
> **`UNVERIFIED (needs QNX SDP 8.0)`**.

---

## 1. Chain of Trust and Process Types

Every Ambre process runs in its own isolated memory space with a dedicated
security type defined in `image/ambre.sec` and mirrored in
`target/port/ambre_port.h` (`ambre_peer_type_t`):

| Process | QNX type (`image/ambre.sec`) | Portable enum (`ambre_port.h`) | Priority (§4.4) | Role |
|---|---|---|---|---|
| `edr-journal` | `edr_journal_t` | `AMBRE_PEER_JOURNAL` (1) | 62 | Sole raw-disk owner (`/dev/hd0t177`), recovery, sealing |
| `edr-capture` | `edr_capture_t` | `AMBRE_PEER_CAPTURE` (2) | 60–61 (61) | Sample ingestion (`/dev/edr/in`), trigger, Class A snapshot |
| `edr-replay` / CAN / zenoh | `edr_producer_t` | `AMBRE_PEER_PRODUCER` (3) | 50 | Input frame producers |
| `edrctl` | `edr_service_t` | `AMBRE_PEER_SERVICE` (4) | 10 | Service CLI: `status`, `list`, `export`, `verify` |
| `edr-archive` | `edr_archive_t` | `AMBRE_PEER_ARCHIVE` (5) | 8 | Reads sealed segments, writes MCAP to the Power-Safe partition `/dev/hd1t177` (`fs-qnx6.so`) |
| Any other process | untyped / intruder | `AMBRE_PEER_UNKNOWN` (99) | — | Denied (`AMBRE_PORT_ERR_PERM` / kernel `EPERM`) |

All five processes drop root privileges (`nonroot` ability) and lock only the
minimum required abilities (`pathspace`, `public_channel`, `rlimit`, `trace` for
`edr-journal` and `edr-capture`; `nonroot` only for clients).

---

## 2. Access-Control Matrix ("Who Can Do What")

The matrix enforced by `image/ambre.sec` (on QNX) and `ambre_port_access_allowed()`
(on PC) is strictly partitioned across the four operations in
`ambre_access_op_t`:

| Peer type | `AMBRE_OP_WRITE_INPUT` (`/dev/edr/in`) | `AMBRE_OP_READ_STATUS` (`/dev/edr/status`) | `AMBRE_OP_READ_EVENTS` (sealed read/export) | `AMBRE_OP_RAW_DISK` (`/dev/hd0t177`) |
|---|---|---|---|---|
| `AMBRE_PEER_JOURNAL` (`edr_journal_t`) | Deny | Deny | Deny | **Allow** |
| `AMBRE_PEER_CAPTURE` (`edr_capture_t`) | Deny | Deny | Deny | Deny |
| `AMBRE_PEER_PRODUCER` (`edr_producer_t`) | **Allow** | Deny | Deny | Deny |
| `AMBRE_PEER_SERVICE` (`edr_service_t`) | Deny | **Allow** | **Allow** | Deny |
| `AMBRE_PEER_ARCHIVE` (`edr_archive_t`) | Deny | **Allow** | **Allow** | Deny |
| `AMBRE_PEER_UNKNOWN` (any other) | Deny | Deny | Deny | Deny |

Notes on internal paths:
- `edr-capture` communicates freeze snapshots (`TRIGGER`, `CONTINUE`, `SEAL`,
  `GAP`) to `edr-journal` over `/dev/edr/journal`, never directly to the raw
  partition (STO-01, SEC-01).
- `edr-archive` writes MCAP files exclusively to the Power-Safe archive
  partition `/dev/hd1t177` (`fs-qnx6.so`, a separate QEMU disk), never to the
  raw journal partition `/dev/hd0t177` (STO-07).

---

## 3. Requirements Coverage (SEC-01..SEC-06, VIE-01, VIE-03)

One line per requirement: **what implements it** (file + function) and **what
proves it** (named test case, or the static check, or the honest statement that
it needs the target). Test case names are the ones printed by the test
binaries; `make BUILD=build-mine test` runs them all.

| ID | Requirement | Implemented in (path → function) | Proven by |
|---|---|---|---|
| **VIE-01** | No VIN, position, absolute wall-clock time or driver identity can be recorded | `target/capture/edr_capture.c` → `ambre_capture_is_forbidden()` (`signal_id >= 90 && <= 99`), `ambre_capture_is_whitelisted()`; the on-disk structs carry no such field (`fmt/ambre_types.h`: `ambre_sample_t` = id/quality/value) | `tests/test_capture.c::capture_whitelist_forbidden` (1 unlisted → `samples_rejected`, 2 forbidden → `samples_forbidden`), 43 checks green; `tests/test_mcap_py.py::test_verify_rejects_an_absolute_timestamp_in_metadata` |
| **VIE-03** | Closed loop: zero network connections | absent by construction: no `io-sock`, `dhclient`, `sshd` or socket library in `image/ambre.build`, `image/buildfile` or `image/ambre.sec` | `make image-check` → check 8 (*no network stack, daemon or socket library in `image/`*), green; QEMU is launched with no `-netdev` (`docs/QNX-PORT.md` §4 step 4) |
| **SEC-01** | One QNX type per process; producers write the input only; the service reads only; the journal alone reaches the raw partition | `image/ambre.sec` (`type`, `provides`, `allow`, `allow_attach`, one type per `ambre_peer_type_t`) + `target/port/ambre_port_posix.c` → `ambre_port_access_allowed()` + its QNX twin `target/port/ambre_port_qnx.c` + `target/journal/edr_journal_qnx.c` (`iofunc_client_info()` → `ambre_port_access_allowed()`) | **Decision function:** `tests/test_port_posix.c::access control (SEC-01)` (16 rows of the full matrix), `tests/test_edrctl.c::access_control_sec01`, `tests/test_journal_svc.c::access_control_and_no_stop` (an `AMBRE_PEER_UNKNOWN` producer gets `AMBRE_PORT_ERR_PERM` + `NAK`). **Policy coherence:** `make image-check` → check 9 (*every `ambre_peer_type_t` mapped*) and check 10 (*no non-journal type granted raw device access*). **Kernel enforcement is `UNVERIFIED (needs QNX SDP 8.0)`** |
| **SEC-02** | No command can disable the recorder; a forced kill triggers respawn and logs the interruption | no `stop`/`disable`/`erase` message exists (`target/edr_ipc.h` lists every type: FRAME, EXTERNAL, TRIGGER, CONTINUE, SEAL, GAP, EVENT_READY, STATUS, LIST, EXPORT, VERIFY, ACK, NAK, …_REPLY) and no such CLI verb in `target/ctl/`; `image/ambre-supervise.sh` respawns and logs to `slog2` | absence: `make image-check` → check 11 (`check_no_disable_command`: *no recorder stop/disable command exists*); unknown opcode refused: `tests/test_journal_svc.c::access_control_and_no_stop` (`0x7F` and a truncated frame both answer `NAK`, `refused_count >= 5`, `malformed_count >= 2`). **Respawn on target: `UNVERIFIED (needs QNX SDP 8.0)`** |
| **SEC-03** | A locked event cannot be erased by any application command | no erase/delete message type in `ambre_ipc_msg_t`; the victim policy never returns a locked segment: `fmt/ambre_fmt_table.c` → `ambre_table_pick_victim()` skips `AMBRE_SEG_LOCKED` and any event with `locked != 0` | `tests/test_table.c::table_pick_victim_policy`, `tests/test_journal.c::locked_event_is_never_recycled`, `tests/test_journal_svc.c::locked_event_cannot_be_erased` (fill the partition, refuse the new event, both locked events still present), `tests/test_recovery.c::locked_event_is_kept`, + invariant I6 over 64 591 explored cut states |
| **SEC-04** | Export carries an integrity envelope, compatible with anonymisation | `host/mcap/export_mcap.py` writes the `edr.integrity` metadata record (`image_sha256`, `image_bytes`, `event_payload_crc32c`, `t0_offset_ns`, `trig_ts_us`); the C++ archive computes the same digest in `target/archive/ambre_sha256.cpp` / `edr_archive_store.cpp` (`source_sha256`, `source_bytes`) | **partial** — `tests/test_archive_cpp.cpp::test_sha256_vectors` (SHA-256 known vectors), `tests/test_mcap_py.py` (metadata present and accepted by `edr_verify.py`); the record-level **hash chain** and the vehicle-type **signature** are still a placeholder: `export_mcap.py` writes `"signature": "none"`, and `edr_verify.py` accepts it. Those two remain to be implemented |
| **SEC-05** | Malformed frames are rejected and counted without affecting the service | `target/ipc/edr_ipc.c` → `ambre_ipc_header_validate()` (magic, version, type, body length) and `ambre_ipc_frame_check()` (whole frame against its declared length); `target/journal/edr_journal_svc.c` counts `refused_count` / `malformed_count` and answers `NAK` | `tests/test_ipc.c` — *reject bad magic*, *reject unknown version*, *reject body above the ceiling*, *reject null header*, *reject incoherent body_len* (102 checks green); `tests/test_journal_svc.c::access_control_and_no_stop` (service keeps serving after refusals); `tests/test_capture.c::capture_overload_non_blocking` |
| **SEC-06** | Read-only configuration in the boot image; digest stamped in every event | `[perms=0444] /etc/ambre/config.conf` in `image/ambre.build`; read-only grants and no write ability on the config path in `image/ambre.sec`; `config_sha256` in the `edr.build` metadata record (`host/mcap/export_mcap.py`) | **partial** — layout: `make image-check` (file list + policy coherence, checks 1, 7, 8). IFS runtime permissions and the per-event digest on target: `UNVERIFIED (needs QNX SDP 8.0)` |

---

## 4. What Is Verified on PC vs. Target-Only

### Verified on PC (`posix` backend + static checks)
1. **Access-control decision logic**: `ambre_port_access_allowed(peer_type, op)`
   in `target/port/ambre_port_posix.c` implements the exact 4-operation matrix
   above and returns `AMBRE_PORT_ERR_PERM` for every unauthorized `(peer_type, op)`
   pair and `AMBRE_PORT_ERR_PARAM` for invalid operations.
   *Measured 2026-11-11:* `./build-doc/test_port_posix` → `access control
   (SEC-01) ok`, `PASSED: 87 checks, 0 failures`; the table in
   `tests/test_port_posix.c` (`acl_rows[]`) has 16 explicit rows.
2. **Service-level enforcement of the same matrix**: the journal service refuses
   the peer, answers `NAK` and counts it —
   `./build-doc/test_journal_svc` → `access_control_and_no_stop ok`,
   `PASSED: 8277 checks, 0 failures`.
3. **Static image & policy coherence (`scripts/check-image.sh` / `make image-check`)**,
   measured `IMAGE CHECK PASSED`, 11/11:
   - Every process in `image/ambre.build` has a matching build target in `Makefile` / `image/buildfile` (checks 2–6).
   - Process priorities in `image/ambre.build` and `image/buildfile` match spec §4.4
     (`edr-capture` 60–61, `edr-journal` 62, `edr-archive` 8, `edrctl` 10, `edr-replay` 50) (check 7).
   - Every `ambre_peer_type_t` in `target/port/ambre_port.h` is mapped in `image/ambre.sec` (check 8).
   - Zero network stack, daemon, or socket library is referenced under `image/` (VIE-03) (check 9).
   - Raw journal partition (`/dev/hd0t177`, disk 0) and Power-Safe archive partition (`/dev/hd1t177`, disk 1) are distinct devices, and `edr-journal` starts before `fs-qnx6.so` (STO-01, STO-07, QNX-07) (check 10).
   - No `stop` or `disable` recorder command exists in `image/` (SEC-02) (check 11).
4. **QNX glue compiles**: `make qnx-syntax` → `qnx-syntax: 5 glue file(s)
   compile against the stubs`, which includes `target/port/ambre_port_qnx.c`
   (slog2, trace, `SchedSet`, `mlockall`), `ambre_port_disk_qnx.c`
   (`devctl(DCMD_FLUSH_CACHE)`), `ambre_port_ipc_qnx.c` (channels, pulses,
   `_PULSE_CODE_DISCONNECT`) and `edr_journal_qnx.c` (`iofunc_client_info()` →
   `ambre_port_access_allowed()`). Syntax only — see the boundary above.

### Target-only — `UNVERIFIED (needs QNX SDP 8.0)`
1. **Kernel enforcement of `image/ambre.sec`**: compiling with `secpolcompile`,
   pushing with `secpolpush`, and verifying that an unauthorized process receives
   `EPERM` from QNX `procnto-smp-instr` when attempting to `open("/dev/hd0t177", ...)`
   or `ConnectAttach()` to `/dev/edr/in`.
2. **Peer identity verification by the kernel**: on the PC POSIX backend, a
   message sender announces its peer type in the IPC header; on QNX, the kernel
   security policy and `procmgr_ability` / `ConnectClientInfo()` bind the peer
   type so a client cannot spoof a higher privilege (`edr_capture_t` or `edr_journal_t`).

---

## 5. Supervision and Migration to QNX HAM (QNX-06, ROB-02/03/04, SEC-02)

In the current repository (built without QNX SDP 8.0), `image/ambre.build`
starts critical processes through `image/ambre-supervise.sh`:
- Sets `AMBRE_SUPERVISED=1` (inspected by `ambre_port_supervised()`).
- Traps `SIGHUP` so accidental session hangups do not kill supervision.
- Logs every start and unexpected exit (`rc=$?`) to `slog2` (OBS-01) and `stderr`.
- Immediately respawns the terminated process (SEC-02, ROB-03, ROB-04).

### Target Replacement Plan (`UNVERIFIED (needs QNX SDP 8.0)`)

On a real QNX 8.0 target with SDP installed, `image/ambre-supervise.sh` **must be
replaced** by the QNX High Availability Manager (`ham`). The exact HAM API
names below are `UNVERIFIED (needs QNX SDP 8.0)`; the mechanism is the one
QNX-06 requires ("processes are launched and relaunched by the QNX supervision
service"):

1. Start the `ham` daemon in `image/ambre.build`, before the EDR processes.
2. Each critical service (`edr-journal`, `edr-capture`, `edr-archive`) becomes
   a HAM entity: it connects to HAM (`ham_connect()`) and registers itself
   (`ham_entity()`), or HAM launches it from a boot-time entity list.
3. Attach a death condition (`ham_condition()`, e.g. the process-death
   condition) with a restart action (`ham_action()`) and a log action, so:
   - `edr-journal` crash -> `ham` restarts `edr-journal` (< 100 ms target, ROB-04);
     startup recovery (`REP-01`) seals any open event before accepting new frames.
   - `edr-capture` crash -> `edr-journal` detects connection close immediately
     via `QNX-05` (`ambre_port_chan_poll_disconnect()`), writes a `GAP` record
     (`ROB-03`), and `ham` restarts `edr-capture`.
   - `edr-archive` crash -> `ham` restarts `edr-archive`; capture and freeze path
     are completely unaffected (`ROB-02`).
4. Remove `image/ambre-supervise.sh` from `startup-script` once `ham` integration
   is compiled and verified against QNX SDP 8.0.
