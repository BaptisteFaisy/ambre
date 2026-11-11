# Power-cut block server — protocol and cache semantics

`faulty_block_server.py` serves the guest disk over the network so that a
power cut can actually lose data. This document specifies what it speaks, what
it guarantees, and where the cache loss comes from.

## 1. Why a network block server

Killing QEMU (`SIGKILL`) is a faithful power cut for the **guest**, but not for
the **host**: writes the guest already handed to the host block layer survive,
because the host page cache and the disk still hold them. Level 2 of the
validation plan (spec §7.4) therefore tests the *ordering* and the
*interruption* of the writes, not the loss of a volatile cache.

When the disk is served by this server instead, the write-back cache lives in
the server's memory:

| Event | Effect on the data |
|---|---|
| guest write | buffered in the volatile overlay — **not** durable |
| guest FLUSH | overlay written to the backing file, `fsync`, overlay cleared |
| power cut | each sector still in the overlay is dropped with probability `--drop-unflushed-ratio`; the others are written to the backing file |

The rest of the write path is unchanged: the guest still uses `cache=none` and
`aio=native` on its side, so the ordering it observes is the ordering the
server received.

## 2. Transport

* `--listen unix:/path/to.sock` — unix stream socket (default), or
* `--listen 127.0.0.1:10809` — TCP.

One export per server (`--export ambre`), raw block addressing, 512-byte
sector granularity internally.

QEMU attaches it with:

```sh
qemu-system-x86_64 ... \
  -drive file=nbd:unix:/path/to.sock,if=none,id=ambre,format=raw,cache=writeback
```

`qemu_vm.py --nbd-socket` builds exactly that line.

## 3. Handshake — NBD fixed newstyle

Standard NBD, big-endian, no TLS, no structured replies.

1. Server → client: `"NBDMAGIC"` (8 bytes), `"IHAVEOPT"` (8 bytes),
   `uint16` handshake flags = `NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES` (`0x0003`).
2. Client → server: `uint32` client flags
   (`NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES`).
3. Option haggling. Each client option is
   `uint64` magic `0x49484156454F5054`, `uint32` option, `uint32` length,
   then `length` bytes of payload. Each reply is
   `uint64` magic `0x0003E889045565A9`, `uint32` option, `uint32` reply type,
   `uint32` length, then the payload.

Supported options:

| Option | Value | Behaviour |
|---|---|---|
| `NBD_OPT_EXPORT_NAME` | 1 | reply is **not** wrapped: `uint64` size, `uint16` transmission flags, then 124 zero bytes unless `NO_ZEROES` was negotiated. Transmission starts. |
| `NBD_OPT_INFO` | 6 | `NBD_REP_INFO` carrying `NBD_INFO_EXPORT` (`uint16` info type, `uint64` size, `uint16` flags), then `NBD_REP_ACK`. Handshake continues. |
| `NBD_OPT_GO` | 7 | same replies as `INFO`, then transmission starts. |
| `NBD_OPT_ABORT` | 2 | `NBD_REP_ACK`, connection closed. |
| anything else | — | `NBD_REP_ERR_UNSUP` (`0x80000003`), handshake continues. |

Transmission flags advertised: `NBD_FLAG_HAS_FLAGS`, `NBD_FLAG_SEND_FLUSH`,
`NBD_FLAG_SEND_FUA`, `NBD_FLAG_SEND_TRIM` (`0x0029`).

## 4. Transmission — simple requests

Request, 28 bytes then optional payload:

```
uint32 magic      = 0x25609513
uint16 flags      (bit 0 = NBD_CMD_FLAG_FUA)
uint16 type
uint64 handle
uint64 offset
uint32 length
[length bytes of data, WRITE only]
```

Reply, 16 bytes then optional payload:

```
uint32 magic      = 0x67446698
uint32 error      (0 = ok, 22 = EINVAL out of the export)
uint64 handle
[length bytes of data, READ only]
```

| Type | Value | Server behaviour |
|---|---|---|
| `NBD_CMD_READ` | 0 | overlay first, backing file second |
| `NBD_CMD_WRITE` | 1 | buffered in the volatile overlay; with `FUA` it is followed by a flush |
| `NBD_CMD_DISC` | 2 | close the connection |
| `NBD_CMD_FLUSH` | 3 | overlay → backing file, `fsync`, journalled |
| `NBD_CMD_TRIM` | 4 | zeroed write, buffered like a normal write |

Writes may be unaligned: the server does read-modify-write at 512-byte sector
granularity.

## 5. Journal

With `--log flushes.jsonl` (or a `log_path` passed to `start_server`), every
event is appended as one JSON object per line:

```json
{"bytes": 512, "event": "flush", "flush_count": 1, "offset": 0,
 "runs": [{"length": 512, "offset": 0}], "seq": 1, "time": 1794357658.647}
{"drop_ratio": 1.0, "dropped_bytes": 512, "dropped_sectors": 1,
 "event": "power_cut", "kept_runs": [], "kept_sectors": 0, "seq": 2, "time": ...}
```

`event: "flush"` is the proof used by essay E1: a FLUSH order really arrived
from the guest. `event: "power_cut"` records what the cut kept and dropped.

## 6. Power cut

* In-process: `server.power_cut(ratio=None)` — drops, persists, journals.
* Standalone: `SIGTERM` or `SIGINT` performs the same cut, then exits 0.

`--drop-unflushed-ratio R` is the probability that a given **sector** still in
the overlay is lost. `R = 1.0` loses everything unflushed (worst case),
`R = 0.5` is a realistic half-written cache, `R = 0.0` keeps everything (the
level-2 behaviour, useful as a control run).

## 7. Reference client

`NbdClient` in `e1_flush_probe.py` is the reference implementation of the
client side (`connect`, `read`, `write`, `flush`, `disconnect`). `flush()`
returns the measured round-trip latency, which is what essay E1 reports.

## 8. Limits and TODO

* One export, raw format, no `NBD_OPT_LIST`, no `NBD_OPT_STRUCTURED_REPLY`, no
  `NBD_CMD_CACHE`, no multi-connection.
* Not specified for concurrent writers: the overlay is protected by a lock but
  the guest is expected to be the only client.
* TODO(extreme-level): serve the *journal* partition rather than the whole
  disk, so a cut can be scoped to the EDR's own writes.
* TODO(extreme-level): replay mode — record every request with its arrival
  time and replay the trace without a guest, for the 10 000-cut target.
