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

| ID | Requirement | Mechanism | Verification Status |
|---|---|---|---|
| **VIE-01** | No VIN, GPS position, absolute wall-clock time, or driver identity | Signal whitelist at `edr-capture` (`ACQ-04`); no RTC/location fields in `ambre_sample_t` or MCAP schema | **Verified on PC** (unit & verify tests) |
| **VIE-03** | Closed loop: zero network connections | No `io-sock`, `dhclient`, `sshd`, `libsocket`, or network type in `image/ambre.build`, `image/buildfile`, or `image/ambre.sec`; QEMU is launched without any network device (`image/README.md` §3) | **Statically checked on PC** (`scripts/check-image.sh`); target runtime `UNVERIFIED (needs QNX SDP 8.0)` |
| **SEC-01** | QNX security policy types; producer writes input only; service reads only; journal alone accesses raw partition | `image/ambre.sec` (`type`, `allow`, `allow_attach`) + `ambre_port_access_allowed()` | Decision function **verified on PC**; kernel enforcement `UNVERIFIED (needs QNX SDP 8.0)` |
| **SEC-02** | No command disables the recorder; forced kill triggers respawn and logs interruption | No `stop` / `disable` / `erase` IPC message (`target/edr_ipc.h`) or CLI command; `image/ambre-supervise.sh` respawns killed processes and logs to `slog2` | IPC/CLI absence **statically checked on PC**; target kill/respawn `UNVERIFIED (needs QNX SDP 8.0)` |
| **SEC-03** | Locked events cannot be erased by any application command | No erase/delete message in `ambre_ipc_msg_t`; allocation table victim policy skips `LOCKED` segments (`fmt/ambre_fmt_table.c`) | **Verified on PC** (`test_table`, `test_explore`) |
| **SEC-04** | Export SHA-256 hash chain + vehicle-type signature (compatible with anonymisation) | `edr.integrity` metadata of `host/mcap/export_mcap.py`: SHA-256 digest of the journal image and of the configuration | **Partially verified on PC** (`test_mcap_py.py`, `edr_verify.py`); the record-level hash chain and the vehicle-type signature are still a placeholder (`"signature": "none"`) and remain to be implemented |
| **SEC-05** | Malformed frames rejected and counted without affecting service | `ambre_ipc_header_validate()` / `ambre_ipc_frame_check()` drop bad frames and increment counters | **Verified on PC** (`test_ipc`, `test_journal_svc`) |
| **SEC-06** | Read-only configuration in boot image; digest stamped in every event | `[perms=0444] /etc/ambre/config.conf` in `image/ambre.build`; read-only grants in `image/ambre.sec` | Layout **statically checked on PC**; IFS runtime permissions and per-event digest `UNVERIFIED (needs QNX SDP 8.0)` |

---

## 4. What Is Verified on PC vs. Target-Only

### Verified on PC (`posix` backend + static checks)
1. **Access-control decision logic**: `ambre_port_access_allowed(peer_type, op)`
   in `target/port/ambre_port_posix.c` implements the exact 4-operation matrix
   above and returns `AMBRE_PORT_ERR_PERM` for every unauthorized `(peer_type, op)`
   pair and `AMBRE_PORT_ERR_PARAM` for invalid operations.
2. **Static image & policy coherence (`scripts/check-image.sh` / `make image-check`)**:
   - Every process in `image/ambre.build` has a matching build target in `Makefile` / `image/buildfile`.
   - Process priorities in `image/ambre.build` and `image/buildfile` match spec §4.4
     (`edr-capture` 60–61, `edr-journal` 62, `edr-archive` 8, `edrctl` 10, `edr-replay` 50).
   - Every `ambre_peer_type_t` in `target/port/ambre_port.h` is mapped in `image/ambre.sec`.
   - Zero network stack, daemon, or socket library is referenced under `image/` (VIE-03).
   - Raw journal partition (`/dev/hd0t177`, disk 0) and Power-Safe archive partition (`/dev/hd1t177`, disk 1) are distinct devices (STO-01, STO-07).
   - No `stop` or `disable` recorder command exists in `image/` (SEC-02).

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
