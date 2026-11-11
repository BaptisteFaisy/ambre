# Chapter 1 — The block format and CRC-32C

*Part of the codelab "Writing a journal that survives power cuts on QNX 8.0,
and proving it". Previous: [README](README.md). Next:
[Chapter 2 — the table in two copies](02-table-two-copies.md).*

**What this chapter proves:** STO-02 (every block carries a magic, a version, a
64-bit sequence number, a session id, a type, a length and a CRC-32C) and
REP-05 (a block whose CRC is invalid is never returned as data).
**Where the proof lives:** `tests/test_crc32c.c`, `tests/test_block.c`.

---

## 1.1 The problem

You call `write()`. The power dies. What is on the medium?

The honest answer is: *you do not know*. The medium may have kept all 512
bytes, none of them, or — the interesting case — the first 300. A
half-written sector is not obviously half-written: you can read back 512 bytes
that look perfectly plausible. Anything you build on top of that ambiguity is
a guess.

So the first piece of the journal is not a buffer, not a queue, not a file: it
is a **unit of storage you can judge**. One sector, 512 bytes, with enough
redundancy inside it to say "these are the bytes I meant to write" or "throw
me away".

## 1.2 The real code — the block structure

`fmt/ambre_types.h` declares the on-disk block. Packed, little-endian,
`memcpy`-only access (never through a pointer cast), so it is identical on the
PC and on QNX:

```c
/* Block header, 48 bytes. A record of N blocks shares seq/chunk_count. */
typedef struct {
    uint32_t magic;            /* AMBRE_MAGIC_BLOCK                           */
    uint16_t version;          /* AMBRE_FORMAT_VERSION                        */
    uint8_t  rec_type;         /* ambre_rec_type_t                            */
    uint8_t  flags;            /* AMBRE_FL_CONTINUATION                       */
    uint64_t seq;              /* record sequence number, monotonic per boot  */
    uint64_t session_id;       /* boot session id, flags stale data           */
    uint64_t first_ts_us;      /* monotonic microseconds, relative (VIE-02)   */
    uint16_t chunk_index;      /* 0-based block index inside the record       */
    uint16_t chunk_count;      /* total blocks in the record, 1..MAX_BLOCKS   */
    uint32_t payload_len;      /* valid bytes in THIS block's payload         */
    uint32_t total_len;        /* valid bytes in the WHOLE record payload     */
    uint32_t payload_crc;      /* CRC-32C over this block's payload_len bytes */
} ambre_block_hdr_t;

/* One 512-byte block: header + payload + whole-block CRC. */
typedef struct {
    ambre_block_hdr_t hdr;                                  /*  48          */
    uint8_t payload[AMBRE_BLOCK_PAYLOAD_MAX];               /* 460          */
    uint32_t block_crc;   /* CRC-32C over the 508 preceding bytes */        /* 4 */
} ambre_block_t;
```

Two checksums, on purpose. `payload_crc` covers the *body*; `block_crc` covers
the *header plus body*. If the header itself is damaged you cannot trust
`payload_len`, so you must not use `payload_crc` — that is what the outer
`block_crc` is for.

## 1.3 The real code — CRC-32C

Castagnoli, the iSCSI/SCTP/Btrfs parameter set: reflected polynomial
`0x82F63B78`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. One static
256-entry table, no allocation (`fmt/ambre_crc32c.c`):

```c
uint32_t ambre_crc32c(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    size_t i;

    for (i = 0; i < len; i++) {
        c = s_crc32c_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}
```

The signature is deliberately the classic `crc32()` chaining form: pass `0`
the first time, then feed back the previous return value. That is not a
cosmetic choice — the journal sometimes assembles a payload in pieces and
checksums it without copying it contiguous, which a one-shot API would forbid
on the critical path.

The check value is public and must be tested **before** anything else:

```
CRC32C("123456789") == 0xE3069283
```

## 1.4 The real code — validation

`fmt/ambre_fmt_block.c` turns a block into a verdict. Note the order: cheap
structural checks first, CRC last, and **one distinct error code per failure
mode** so a bug report can say what actually happened:

```c
int ambre_block_validate(const ambre_block_t *b)
{
    uint32_t expected_block_crc;
    uint32_t expected_payload_crc;

    if (b == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (b->hdr.magic != AMBRE_MAGIC_BLOCK) {
        return AMBRE_ERR_BAD_MAGIC;
    }
    if (b->hdr.version != AMBRE_FORMAT_VERSION) {
        return AMBRE_ERR_BAD_VER;
    }
    if (b->hdr.chunk_count == 0u) {
        return AMBRE_ERR_PARAM;      /* a record always spans >= 1 block */
    }
    if (b->hdr.chunk_count > AMBRE_REC_MAX_BLOCKS ||
        b->hdr.payload_len > AMBRE_BLOCK_PAYLOAD_MAX ||
        b->hdr.total_len > (uint32_t)AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_TOO_BIG;
    }
    if (b->hdr.chunk_index >= b->hdr.chunk_count) {
        return AMBRE_ERR_PARAM;
    }

    expected_block_crc = compute_block_crc(b);
    if (b->block_crc != expected_block_crc) {
        return AMBRE_ERR_BAD_CRC;
    }

    expected_payload_crc = ambre_crc32c(0u, b->payload, b->hdr.payload_len);
    if (b->hdr.payload_crc != expected_payload_crc) {
        return AMBRE_ERR_BAD_CRC;
    }
    return AMBRE_OK;
}
```

And the whole-block CRC is computed over everything *except* the CRC field
itself:

```c
static uint32_t compute_block_crc(const ambre_block_t *b)
{
    uint8_t raw[AMBRE_BLOCK_SIZE];
    ambre_block_pack(b, raw);
    return ambre_crc32c(0u, raw, AMBRE_BLOCK_SIZE - AMBRE_BLOCK_CRC_SIZE);
}
```

`ambre_block_pack()` is a `memcpy` — no cast, no padding surprise, no
endianness trap on a big-endian host.

## 1.5 The rule that makes a torn record detectable

- A record longer than one block occupies **consecutive** blocks and is
  submitted as **one contiguous write**, so the medium sees one transaction
  instead of a sequence of independent sectors (GEL-01, chapter 4).
- `chunk_index` / `chunk_count` say where each block belongs; on read the
  chunks are gathered and the bytes must reassemble to `total_len`.
- **A record is valid only if every chunk is valid.** Otherwise the whole
  record is torn and is dropped. Half an event is worse than no event: it
  would be reported as data that was never fully recorded.

## 1.6 Run it

```sh
make BUILD=build-mine all
./build-mine/test_crc32c
./build-mine/test_block
```

Expected output (measured on this repository, 2026-11-11):

```
PASSED: 85 checks, 0 failures      # test_crc32c
PASSED: 118 checks, 0 failures     # test_block
```

What to observe:

- `crc32c_check_value` — the constant `0xE3069283` comes out;
- `crc32c_incremental_vs_oneshot` — chaining gives the same value as one call;
- `crc32c_single_bit_flip` — one flipped bit changes the checksum;
- `block_corruption_error_codes` — a bad magic, a bad version and a bad CRC
  each return a **different** error;
- `block_multiblock_reassembly_io` — a multi-block record survives a real
  write/read cycle through the disk vtable and reassembles exactly.

If your checksum does not produce the check value, stop here: every later
failure would be unreadable.

---

Next: [Chapter 2 — the table in two copies](02-table-two-copies.md).
