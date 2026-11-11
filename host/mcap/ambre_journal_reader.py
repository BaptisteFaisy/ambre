"""Ambre journal image reader — pure Python 3, standard library only.

Reads a raw journal partition image as specified by docs/FORMAT.md and
fmt/ambre_types.h: both allocation-table copies, the segments, the 512-byte
blocks (magic, version, block CRC, payload CRC), the reassembly of multi-chunk
records, and the decoding of the EMERG/CONT/SEAL/DATA/GAP payloads.

Every structure mirrors a packed C struct byte for byte: little-endian, no
padding, accessed through `struct` only. The CRC-32C code lives here and only
here, so the MCAP writer and the tests share one implementation. Nothing is
invented: a block whose magic, version, length or CRC is wrong is dropped, and
a record whose chunks do not reassemble is dropped as torn (FORMAT.md §2, §10).
"""

import struct

BLOCK_SIZE = 512
BLOCK_HDR_SIZE = 48
BLOCK_CRC_SIZE = 4
BLOCK_PAYLOAD_MAX = BLOCK_SIZE - BLOCK_HDR_SIZE - BLOCK_CRC_SIZE      # 460
REC_MAX_BLOCKS = 16

SEGMENT_SIZE = 2 * 1024 * 1024
SEGMENT_COUNT = 24
BLOCKS_PER_SEGMENT = SEGMENT_SIZE // BLOCK_SIZE                       # 4096
TABLE_A_OFFSET = 0
TABLE_B_OFFSET = BLOCK_SIZE
FIRST_DATA_OFFSET = 2 * BLOCK_SIZE
JOURNAL_BYTES = FIRST_DATA_OFFSET + SEGMENT_COUNT * SEGMENT_SIZE

MAGIC_BLOCK = 0x414D4252          # "AMBR"
MAGIC_TABLE = 0x414D5442          # "AMTB"
FORMAT_VERSION = 1
MAX_EVENTS = 8
EMERG_POINT_COUNT = 21
CONT_POINT_COUNT = 4

REC_DATA, REC_EMERG, REC_CONT, REC_SEAL, REC_GAP = 1, 2, 3, 4, 5

SEG_FREE, SEG_CURRENT, SEG_FROZEN, SEG_LOCKED, SEG_ARCHIVED = 0, 1, 2, 3, 4
EV_EMPTY, EV_OPEN, EV_SEALED = 0, 1, 2
SEAL_NORMAL, SEAL_POWERCUT, SEAL_VOLTAGE, SEAL_OVERWRITE = 0, 1, 2, 3
SEAL_CAUSE_NAMES = {SEAL_NORMAL: "normal", SEAL_POWERCUT: "powercut",
                    SEAL_VOLTAGE: "voltage", SEAL_OVERWRITE: "overwrite"}
EV_STATE_NAMES = {EV_EMPTY: "empty", EV_OPEN: "open", EV_SEALED: "sealed"}

BLOCK_HDR = struct.Struct("<IHBBQQQHHIII")        # 48: header + payload_crc
SAMPLE = struct.Struct("<HBBi")                   # 8: class B sample
A_POINT = struct.Struct("<HBBHhhhBBBBhH")         # 20: class A point
EMERG_HDR = struct.Struct("<IBBBBQQIHH")          # 32, then points[]
CONT_HDR = struct.Struct("<IHHii")                # 16, then points[]
SEAL = struct.Struct("<IBBBBIiHiHHHI")            # 32
GAP = struct.Struct("<QQI")                       # 20
EV_DESC = struct.Struct("<IIBBBBIIIIQQQ")         # 52
TABLE_HDR = struct.Struct("<IIIIQQQIIII24s")      # 80, then events[], res, crc

A_POINT_KEYS = ("speed_kmh_x4", "throttle_pct", "brake_active", "engine_rpm",
                "steer_angle_deg_x2", "accel_long_mm_s2", "accel_lat_mm_s2",
                "abs_active", "esc_active", "belt_driver", "airbag_lamp",
                "yaw_rate_deg_s_x10", "rel_time_ms")
SEAL_KEYS = ("event_id", "file_complete", "seal_cause", "locked", "event_number",
             "time_since_first_ms", "max_dv_long_mm_s", "max_dv_long_at_ms",
             "max_dv_lat_mm_s", "max_dv_lat_at_ms", "restraint_deploy_ms",
             "vru_deploy_ms", "ignition_cycle_at_read")

# Sizes mirrored from the _Static_asserts of fmt/ambre_types.h.
EXPECTED_SIZES = {
    "block header": (BLOCK_HDR.size, BLOCK_HDR_SIZE),
    "class B sample": (SAMPLE.size, 8),
    "class A point": (A_POINT.size, 20),
    "SEAL payload": (SEAL.size, 32),
    "GAP payload": (GAP.size, 20),
    "event descriptor": (EV_DESC.size, 52),
    "EMERG payload": (EMERG_HDR.size + EMERG_POINT_COUNT * A_POINT.size, 452),
    "CONT payload": (CONT_HDR.size + CONT_POINT_COUNT * A_POINT.size, 96),
    "allocation table": (TABLE_HDR.size + MAX_EVENTS * EV_DESC.size + 16, 512),
}


def check_layout():
    """Assert that this module still matches the C header byte for byte."""
    for name, (got, want) in EXPECTED_SIZES.items():
        if got != want:
            raise AssertionError("%s is %d bytes, expected %d" % (name, got, want))
    for name in ("EMERG payload", "CONT payload", "SEAL payload"):
        if EXPECTED_SIZES[name][0] > BLOCK_PAYLOAD_MAX:
            raise AssertionError("%s does not fit in one block" % name)
    return EXPECTED_SIZES


_POLY = 0x82F63B78            # Castagnoli, reflected
_CHECK_VALUE = 0xE3069283     # CRC32C(b"123456789")


def _build_tables():
    base = []
    for index in range(256):
        crc = index
        for _ in range(8):
            crc = (crc >> 1) ^ _POLY if crc & 1 else crc >> 1
        base.append(crc & 0xFFFFFFFF)
    tables = [base]
    for _ in range(7):
        previous = tables[-1]
        tables.append([((c >> 8) ^ base[c & 0xFF]) & 0xFFFFFFFF for c in previous])
    return tables


_TABLES = _build_tables()


def _crc32c_python(data, crc=0):
    """Slicing-by-8 CRC-32C; init and final XOR are 0xFFFFFFFF."""
    crc = (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF
    t0, t1, t2, t3, t4, t5, t6, t7 = _TABLES
    view = memoryview(data).cast("B")
    pos, end = 0, len(view)
    while end - pos >= 8:
        low = int.from_bytes(view[pos:pos + 4], "little")
        high = int.from_bytes(view[pos + 4:pos + 8], "little")
        pos += 8
        crc ^= low
        crc = (t7[crc & 0xFF] ^ t6[(crc >> 8) & 0xFF] ^ t5[(crc >> 16) & 0xFF]
               ^ t4[(crc >> 24) & 0xFF] ^ t3[high & 0xFF]
               ^ t2[(high >> 8) & 0xFF] ^ t1[(high >> 16) & 0xFF]
               ^ t0[(high >> 24) & 0xFF])
    while pos < end:
        crc = (crc >> 8) ^ t0[(crc ^ view[pos]) & 0xFF]
        pos += 1
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


try:                                     # optional accelerator, if installed
    import crc32c as _crc32c_module
except ImportError:                      # pragma: no cover - depends on host
    _crc32c_module = None
else:                                    # pragma: no cover - depends on host
    try:
        if _crc32c_module.crc32c(b"123456789") != _CHECK_VALUE:
            _crc32c_module = None
    except Exception:                    # noqa: BLE001 - any API mismatch
        _crc32c_module = None


def crc32c(data, crc=0):
    """CRC-32C (Castagnoli) of `data`, optionally continuing from `crc`."""
    if _crc32c_module is not None:
        return _crc32c_module.crc32c(bytes(data), crc) & 0xFFFFFFFF
    return _crc32c_python(data, crc)


def crc32c_available_accelerator():
    """True when a compiled crc32c module was found and self-checked."""
    return _crc32c_module is not None


def _block_crc_ok(raw):
    """True when the trailing block_crc covers the first 508 bytes."""
    stored = struct.unpack_from("<I", raw, BLOCK_SIZE - BLOCK_CRC_SIZE)[0]
    return stored == crc32c(raw[:BLOCK_SIZE - BLOCK_CRC_SIZE])


def decode_block(raw):
    """Decode one 512-byte block; None when magic/version/CRCs/lengths fail."""
    if raw is None or len(raw) != BLOCK_SIZE:
        return None
    (magic, version, rec_type, flags, seq, session_id, first_ts_us,
     chunk_index, chunk_count, payload_len, total_len,
     payload_crc) = BLOCK_HDR.unpack_from(raw, 0)
    if magic != MAGIC_BLOCK or version != FORMAT_VERSION:
        return None
    if not 1 <= chunk_count <= REC_MAX_BLOCKS or chunk_index >= chunk_count:
        return None
    if payload_len > BLOCK_PAYLOAD_MAX:
        return None
    if total_len > REC_MAX_BLOCKS * BLOCK_PAYLOAD_MAX or not _block_crc_ok(raw):
        return None
    payload = raw[BLOCK_HDR_SIZE:BLOCK_HDR_SIZE + payload_len]
    if crc32c(payload) != payload_crc:
        return None
    return {"magic": magic, "version": version, "rec_type": rec_type,
            "flags": flags, "seq": seq, "session_id": session_id,
            "first_ts_us": first_ts_us, "chunk_index": chunk_index,
            "chunk_count": chunk_count, "payload_len": payload_len,
            "total_len": total_len, "payload": payload}


def decode_a_point(raw, off=0):
    return dict(zip(A_POINT_KEYS, A_POINT.unpack_from(raw, off)))


def decode_event_desc(raw, off=0):
    (event_id, order, state, locked, file_complete, trig_reason, start_seg,
     start_block, end_seg, end_block, trig_ts_us, seq_emerg,
     seq_last) = EV_DESC.unpack_from(raw, off)
    return {"event_id": event_id, "order": order, "state": state,
            "locked": locked, "file_complete": file_complete,
            "trig_reason": trig_reason, "start_seg": start_seg,
            "start_block": start_block, "end_seg": end_seg,
            "end_block": end_block, "trig_ts_us": trig_ts_us,
            "seq_emerg": seq_emerg, "seq_last": seq_last}


def decode_table(raw):
    """Decode one 512-byte allocation table copy; None when invalid."""
    if raw is None or len(raw) != BLOCK_SIZE:
        return None
    (magic, version, generation, flags, session_id, last_seq, next_event_id,
     current_seg, current_block, event_count, sealed_count,
     seg_states) = TABLE_HDR.unpack_from(raw, 0)
    if magic != MAGIC_TABLE or version != FORMAT_VERSION or not _block_crc_ok(raw):
        return None
    events = [decode_event_desc(raw, TABLE_HDR.size + i * EV_DESC.size)
              for i in range(MAX_EVENTS)]
    return {"magic": magic, "version": version, "generation": generation,
            "flags": flags, "session_id": session_id, "last_seq": last_seq,
            "next_event_id": next_event_id, "current_seg": current_seg,
            "current_block": current_block, "event_count": event_count,
            "sealed_count": sealed_count, "seg_states": list(seg_states),
            "events": events}


def decode_emerg(payload):
    """EMERG payload: the class A pre-crash snapshot of one event."""
    if len(payload) < EMERG_HDR.size:
        return None
    (event_id, trig_reason, locked, event_number, ignition_cycle, t0_ts_us,
     trig_ts_us, crash_order, point_count, _reserved) = EMERG_HDR.unpack_from(payload, 0)
    count = min(point_count, EMERG_POINT_COUNT)
    base = EMERG_HDR.size
    if len(payload) < base + count * A_POINT.size:
        return None
    return {"event_id": event_id, "trig_reason": trig_reason, "locked": locked,
            "event_number": event_number, "ignition_cycle": ignition_cycle,
            "t0_ts_us": t0_ts_us, "trig_ts_us": trig_ts_us,
            "crash_order": crash_order, "point_count": count,
            "points": [decode_a_point(payload, base + i * A_POINT.size)
                       for i in range(count)]}


def decode_cont(payload):
    """CONT payload: class A points plus cumulative delta-v during the event."""
    if len(payload) < CONT_HDR.size:
        return None
    event_id, sample_count, rel_time_ms, dv_long, dv_lat = CONT_HDR.unpack_from(payload, 0)
    count = min(sample_count, CONT_POINT_COUNT)
    base = CONT_HDR.size
    if len(payload) < base + count * A_POINT.size:
        return None
    return {"event_id": event_id, "sample_count": count,
            "rel_time_ms": rel_time_ms, "dv_long_mm_s": dv_long,
            "dv_lat_mm_s": dv_lat,
            "points": [decode_a_point(payload, base + i * A_POINT.size)
                       for i in range(count)]}


def decode_seal(payload):
    """SEAL payload: end-of-event summary."""
    if len(payload) < SEAL.size:
        return None
    return dict(zip(SEAL_KEYS, SEAL.unpack_from(payload, 0)))


def decode_gap(payload):
    """GAP payload: one capture interruption."""
    if len(payload) < GAP.size:
        return None
    start_ts_us, end_ts_us, reason = GAP.unpack_from(payload, 0)
    return {"start_ts_us": start_ts_us, "end_ts_us": end_ts_us, "reason": reason}


def decode_samples(payload):
    """DATA payload: a packed array of 8-byte class B samples."""
    out = []
    for index in range(len(payload) // SAMPLE.size):
        signal_id, quality, pad, value = SAMPLE.unpack_from(payload, index * SAMPLE.size)
        out.append({"signal_id": signal_id, "quality": quality, "pad": pad,
                    "value": value})
    return out


class JournalReader:
    """Random access over a raw journal image (file path or bytes)."""

    def __init__(self, path=None, data=None):
        if data is None:
            if path is None:
                raise ValueError("JournalReader needs a path or raw data")
            with open(path, "rb") as handle:
                data = handle.read()
        self.data = data
        self._table = None
        self._records = None

    def slice(self, offset, length):
        if offset < 0 or length < 0 or offset + length > len(self.data):
            return None
        return self.data[offset:offset + length]

    @staticmethod
    def block_offset(seg, blk):
        return FIRST_DATA_OFFSET + seg * SEGMENT_SIZE + blk * BLOCK_SIZE

    def read_block(self, seg, blk):
        if not 0 <= seg < SEGMENT_COUNT or not 0 <= blk < BLOCKS_PER_SEGMENT:
            return None
        return decode_block(self.slice(self.block_offset(seg, blk), BLOCK_SIZE))

    def table(self):
        """Return (table, copy_index): the valid copy with highest generation."""
        if self._table is None:
            best, best_copy = None, -1
            for index, offset in enumerate((TABLE_A_OFFSET, TABLE_B_OFFSET)):
                table = decode_table(self.slice(offset, BLOCK_SIZE))
                if table is None:
                    continue
                if best is None or table["generation"] > best["generation"]:
                    best, best_copy = table, index
            self._table = (best, best_copy)
        return self._table

    def used_segments(self):
        table, _ = self.table()
        if table is None:
            return list(range(SEGMENT_COUNT))
        used = [s for s in range(SEGMENT_COUNT) if table["seg_states"][s] != SEG_FREE]
        return used or list(range(SEGMENT_COUNT))

    def _read_record(self, seg, blk, first):
        """Reassemble the record starting at (seg, blk); None when torn."""
        count = first["chunk_count"]
        parts = []
        for index in range(count):
            chunk = first if index == 0 else self.read_block(seg, blk + index)
            if chunk is None:
                return None
            if (chunk["chunk_index"] != index or chunk["chunk_count"] != count
                    or chunk["seq"] != first["seq"]
                    or chunk["rec_type"] != first["rec_type"]
                    or chunk["total_len"] != first["total_len"]):
                return None
            parts.append(chunk["payload"])
        payload = b"".join(parts)[:first["total_len"]]
        if len(payload) != first["total_len"]:
            return None
        return {"seg": seg, "blk": blk, "seq": first["seq"],
                "session_id": first["session_id"],
                "rec_type": first["rec_type"], "flags": first["flags"],
                "first_ts_us": first["first_ts_us"], "chunk_count": count,
                "total_len": first["total_len"], "payload": payload}

    def records(self):
        """Every valid record of the journal, deduplicated by seq, seq-ordered."""
        if self._records is None:
            self._records = self._scan()
        return self._records

    def _scan(self):
        found, seen = [], set()
        for seg in self.used_segments():
            blk = 0
            while blk < BLOCKS_PER_SEGMENT:
                block = self.read_block(seg, blk)
                if block is None or block["chunk_index"] != 0:
                    blk += 1
                    continue
                record = self._read_record(seg, blk, block)
                if record is None:
                    blk += 1
                    continue
                if record["seq"] not in seen:
                    seen.add(record["seq"])
                    found.append(record)
                blk += record["chunk_count"]
        found.sort(key=lambda rec: (rec["seq"], rec["seg"], rec["blk"]))
        return found

    def events(self):
        table, _ = self.table()
        return [] if table is None else [ev for ev in table["events"]
                                         if ev["state"] != EV_EMPTY]

    def find_event(self, event_id):
        for event in self.events():
            if event["event_id"] == event_id:
                return event
        return None

    @staticmethod
    def _belongs(record, desc):
        """True when `record` falls inside the descriptor's seq window."""
        first, last = desc["seq_emerg"], desc["seq_last"]
        if record["seq"] == first or last < first:      # open event: no end yet
            return record["seq"] >= first
        return first <= record["seq"] <= last

    def read_event(self, event_id):
        """Return an EventView for `event_id` (KeyError when unknown)."""
        desc = self.find_event(event_id)
        if desc is None:
            raise KeyError("event %d is not in the allocation table" % event_id)
        return EventView(desc, [rec for rec in self.records()
                                if self._belongs(rec, desc)])

    def first_event(self):
        """First sealed event, else the first acquired (open) one, else None."""
        events = self.events()
        for event in events:
            if event["state"] == EV_SEALED:
                return event
        return events[0] if events else None


class EventView:
    """One event: its descriptor plus every record that belongs to it."""

    def __init__(self, desc, records):
        self.desc = desc
        self.records = records
        self.emerg = None
        self.seal = None
        self.conts = []      # (block first_ts_us, decoded CONT)
        self.data = []       # (block first_ts_us, [class B samples])
        self.gaps = []
        self._decode()
        self.t0_ts_us = self.emerg["t0_ts_us"] if self.emerg else desc["trig_ts_us"]

    def _decode(self):
        for record in self.records:
            kind, payload = record["rec_type"], record["payload"]
            if kind == REC_EMERG and self.emerg is None:
                self.emerg = decode_emerg(payload)
            elif kind == REC_CONT:
                cont = decode_cont(payload)
                if cont is not None:
                    self.conts.append((record["first_ts_us"], cont))
            elif kind == REC_SEAL and self.seal is None:
                self.seal = decode_seal(payload)
            elif kind == REC_DATA:
                self.data.append((record["first_ts_us"], decode_samples(payload)))
            elif kind == REC_GAP:
                gap = decode_gap(payload)
                if gap is not None:
                    self.gaps.append(gap)

    @property
    def event_id(self):
        return self.desc["event_id"]

    @property
    def locked(self):
        return bool(self.desc["locked"])

    @property
    def file_complete(self):
        """I7: a sealed event reports its seal, an unsealed one its descriptor."""
        if self.seal is not None:
            return bool(self.seal["file_complete"])
        return bool(self.desc["file_complete"])

    @property
    def sealed(self):
        return self.seal is not None or self.desc["state"] == EV_SEALED

    @property
    def point_count(self):
        return self.emerg["point_count"] if self.emerg else 0

    def precrash_points(self):
        """Class A points of the EMERG snapshot, oldest first."""
        if self.emerg is None:
            return []
        return sorted(self.emerg["points"], key=lambda point: -point["rel_time_ms"])

    def cont_records(self):
        """CONT records ordered by their offset from T0."""
        return sorted(self.conts, key=lambda item: item[1]["rel_time_ms"])

    def sample_count(self):
        return sum(len(samples) for _, samples in self.data)


check_layout()      # fail loudly if this module ever drifts from ambre_types.h
