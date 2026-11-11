"""Minimal MCAP writer and reader — pure Python 3, standard library only.

Implements the subset of https://mcap.dev/spec needed by the Ambre EDR export
(docs/FORMAT.md §12, cahier §6.4): header, schemas, channels, messages, chunks
with CRC-32C, DataEnd, a summary section (schema records, channel records,
statistics, chunk indexes, metadata indexes, summary offsets) and the footer.
Encoding is `json` with JSON schemas, compression is none.

Both directions live here so the record codec exists exactly once: the writer
encodes, the reader decodes, and host/verify/edr_verify.py reuses the reader.

Assumptions taken on the spec (not restated in FORMAT.md, network unavailable;
each is enforced by our own reader and matches the reference implementations):
 1. MessageIndex `offset` is relative to the start of the chunk's `records`
    field, not to the chunk record: with empty compression a message sits at
    chunk_start_offset + 9 + 36 + offset (28 bytes of times/sizes, a 4-byte
    compression string, a 4-byte records length). Only reading that survives
    decompression, which yields the records field alone.
 2. Chunk `uncompressed_crc` is the CRC-32C of the uncompressed records field.
 3. `data_section_crc` covers every byte from the file magic through the
    DataEnd record's opcode and length fields.
 4. `summary_crc` covers the summary records, first Schema record up to
    (excluding) the first SummaryOffset record.
 5. Maps, bytes and strings are uint32 byte lengths followed by their content.
 6. Records inside a chunk are Message records with their own opcode+length.
"""

import io
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ambre_journal_reader import crc32c                      # noqa: E402

MAGIC = b"\x89MCAP0\r\n"

OP_HEADER = 0x01
OP_FOOTER = 0x02
OP_SCHEMA = 0x03
OP_CHANNEL = 0x04
OP_MESSAGE = 0x05
OP_CHUNK = 0x06
OP_MESSAGE_INDEX = 0x07
OP_CHUNK_INDEX = 0x08
OP_STATISTICS = 0x09
OP_METADATA = 0x0C
OP_METADATA_INDEX = 0x0D
OP_SUMMARY_OFFSET = 0x0E
OP_DATA_END = 0x0F

class McapError(Exception):
    """Raised when a file does not follow the MCAP framing."""


# ------------------------------------------------------------- encoding -----

def _u16(value):
    return struct.pack("<H", value)

def _u32(value):
    return struct.pack("<I", value)

def _u64(value):
    return struct.pack("<Q", value)


def _string(text):
    raw = text.encode("utf-8")
    return _u32(len(raw)) + raw


def _bytes(raw):
    return _u32(len(raw)) + raw


def _string_map(mapping):
    body = b"".join(_string(str(key)) + _string(str(value))
                    for key, value in mapping.items())
    return _u32(len(body)) + body


def _u16_map(mapping):
    body = b"".join(_u16(key) + _u64(value) for key, value in mapping.items())
    return _u32(len(body)) + body


def record(opcode, body):
    """Frame one MCAP record: opcode, uint64 length, body."""
    return struct.pack("<BQ", opcode, len(body)) + body


# --------------------------------------------------------------- writer -----

class McapWriter:
    """Writes one MCAP file: chunks, summary section and statistics."""

    def __init__(self, path, profile="ambre", library="ambre-host-mcap",
                 chunk_size=512 * 1024):
        self.path = path
        self.profile = profile
        self.library = library
        self.chunk_size = chunk_size
        self._schemas = []           # (id, encoded record)
        self._channels = []          # (id, encoded record)
        self._next_schema_id = 1
        self._next_channel_id = 1
        self._channel_counts = {}    # channel_id -> message count
        self._sequences = {}         # channel_id -> next sequence
        self._chunk_records = []     # encoded Message records of the open chunk
        self._chunk_bytes = 0        # size of the open chunk, encoded
        self._chunk_index = {}       # channel_id -> [(log_time, offset)]
        self._chunks = []            # chunk index entries (dicts)
        self._metadata = []          # (name, encoded record, offset, length)
        self._data = io.BytesIO()
        self._data.write(record(OP_HEADER,
                                _string(self.profile) + _string(self.library)))
        self._message_count = 0
        self._first_time = None
        self._last_time = None

    # -- schemas and channels ----------------------------------------------
    def add_schema(self, name, encoding, data):
        schema_id = self._next_schema_id
        self._next_schema_id += 1
        body = _u16(schema_id) + _string(name) + _string(encoding) + _bytes(data)
        self._schemas.append((schema_id, record(OP_SCHEMA, body)))
        return schema_id

    def add_channel(self, topic, message_encoding, schema_id, metadata=None):
        channel_id = self._next_channel_id
        self._next_channel_id += 1
        body = (_u16(channel_id) + _u16(schema_id) + _string(topic)
                + _string(message_encoding) + _string_map(metadata or {}))
        self._channels.append((channel_id, record(OP_CHANNEL, body)))
        self._channel_counts[channel_id] = 0
        self._sequences[channel_id] = 0
        return channel_id

    def add_metadata(self, name, mapping):
        body = _string(name) + _string_map(mapping)
        self._metadata.append((name, record(OP_METADATA, body)))

    # -- messages -----------------------------------------------------------
    def add_message(self, channel_id, log_time, data, publish_time=None,
                    sequence=None):
        if sequence is None:
            sequence = self._sequences[channel_id]
            self._sequences[channel_id] = sequence + 1
        if publish_time is None:
            publish_time = log_time
        body = (struct.pack("<HIQQ", channel_id, sequence & 0xFFFFFFFF,
                            log_time, publish_time) + _bytes(data))
        encoded = record(OP_MESSAGE, body)
        self._chunk_index.setdefault(channel_id, []).append((log_time, self._chunk_bytes))
        self._chunk_records.append(encoded)
        self._chunk_bytes += len(encoded)
        self._message_count += 1
        self._channel_counts[channel_id] += 1
        first = self._first_time
        last = self._last_time
        self._first_time = log_time if first is None else min(first, log_time)
        self._last_time = log_time if last is None else max(last, log_time)
        if self._chunk_bytes >= self.chunk_size:
            self._flush_chunk()

    # -- chunk and summary --------------------------------------------------
    def _flush_chunk(self):
        if not self._chunk_records:
            return
        records = b"".join(self._chunk_records)
        times = [time for entries in self._chunk_index.values()
                 for time, _ in entries]
        body = (_u64(min(times)) + _u64(max(times)) + _u64(len(records))
                + _u32(crc32c(records)) + _string("") + _bytes(records))
        chunk = record(OP_CHUNK, body)
        entry = {"chunk_start_offset": self._data.tell() + len(MAGIC),
                 "chunk_length": len(chunk), "message_start_time": min(times),
                 "message_end_time": max(times), "uncompressed_size": len(records),
                 "compressed_size": len(records), "message_index_offsets": {},
                 "message_index_length": 0}
        self._data.write(chunk)
        first_index = None
        for channel_id in sorted(self._chunk_index):
            entries = self._chunk_index[channel_id]
            body = _u16(channel_id) + b"".join(
                _u64(time) + _u64(offset) for time, offset in entries)
            if first_index is None:
                first_index = self._data.tell()
            entry["message_index_offsets"][channel_id] = self._data.tell() + len(MAGIC)
            self._data.write(record(OP_MESSAGE_INDEX, body))
        if first_index is not None:
            entry["message_index_length"] = self._data.tell() - first_index
        self._chunks.append(entry)
        self._chunk_records = []
        self._chunk_bytes = 0
        self._chunk_index = {}

    def _write_summary(self, summary):
        """Append schema/channel/statistics/index records; return groups."""
        groups = []

        def group(opcode, items):
            if not items:
                return
            start = summary.tell()
            for item in items:
                summary.write(item)
            groups.append((opcode, start, summary.tell() - start))

        group(OP_SCHEMA, [item for _, item in self._schemas])
        group(OP_CHANNEL, [item for _, item in self._channels])
        group(OP_STATISTICS, [self._statistics_record()])
        chunk_index_records = []
        for entry in self._chunks:
            body = (_u64(entry["message_start_time"]) + _u64(entry["message_end_time"])
                    + _u64(entry["chunk_start_offset"]) + _u64(entry["chunk_length"])
                    + _u16_map(entry["message_index_offsets"])
                    + _u64(entry["message_index_length"]) + _string("")
                    + _u64(entry["compressed_size"]) + _u64(entry["uncompressed_size"]))
            chunk_index_records.append(record(OP_CHUNK_INDEX, body))
        group(OP_CHUNK_INDEX, chunk_index_records)
        metadata_indexes = []
        for name, _, offset, length in self._metadata:
            body = _u64(offset) + _u64(length) + _string(name)
            metadata_indexes.append(record(OP_METADATA_INDEX, body))
        group(OP_METADATA_INDEX, metadata_indexes)
        return groups

    def _statistics_record(self):
        body = (_u64(self._message_count) + _u16(len(self._schemas))
                + _u32(len(self._channels)) + _u32(0) + _u32(len(self._metadata))
                + _u32(len(self._chunks))
                + _u64(self._first_time or 0) + _u64(self._last_time or 0)
                + _u16_map(self._channel_counts))
        return record(OP_STATISTICS, body)

    def finish(self):
        """Write the whole file: data section, summary section, footer."""
        metadata_offsets = []
        for name, encoded in self._metadata:
            metadata_offsets.append((name, encoded,
                                     self._data.tell() + len(MAGIC), len(encoded)))
            self._data.write(encoded)
        self._flush_chunk()
        self._metadata = metadata_offsets
        data_section = self._data.getvalue()
        data_end = record(OP_DATA_END, _u32(crc32c(
            data_section + struct.pack("<BQ", OP_DATA_END, 4))))
        summary = io.BytesIO()
        groups = self._write_summary(summary)
        summary_body = summary.getvalue()
        summary_crc = crc32c(summary_body)
        offsets = io.BytesIO()
        for opcode, start, length in groups:
            offsets.write(record(OP_SUMMARY_OFFSET,
                                 struct.pack("<BQQ", opcode, start, length)))
        summary_offsets = offsets.getvalue()

        summary_start = len(MAGIC) + len(data_section) + len(data_end)
        summary_offset_start = summary_start + len(summary_body)
        footer = record(OP_FOOTER, _u64(summary_start) + _u64(summary_offset_start)
                        + _u32(summary_crc))
        with open(self.path, "wb") as handle:
            handle.write(b"".join((MAGIC, data_section, data_end, summary_body,
                                   summary_offsets, footer, MAGIC)))
        return self.path


# --------------------------------------------------------------- reader -----

def iter_records(buf, start, end):
    """Yield (opcode, body, record_offset, record_length) in [start, end)."""
    pos = start
    while pos + 9 <= end:
        opcode, length = struct.unpack_from("<BQ", buf, pos)
        if pos + 9 + length > end:
            raise McapError("record at %d overruns its section" % pos)
        yield opcode, buf[pos + 9:pos + 9 + length], pos, 9 + length
        pos += 9 + length


def decode_string(buf, pos):
    length = struct.unpack_from("<I", buf, pos)[0]
    return buf[pos + 4:pos + 4 + length].decode("utf-8"), pos + 4 + length


def decode_string_map(buf, pos):
    size = struct.unpack_from("<I", buf, pos)[0]
    pos += 4
    end = pos + size
    out = {}
    while pos < end:
        key, pos = decode_string(buf, pos)
        value, pos = decode_string(buf, pos)
        out[key] = value
    return out, end


def decode_u16_map(buf, pos):
    """Read a uint32-length map of uint16 -> uint64; returns (map, end)."""
    size = struct.unpack_from("<I", buf, pos)[0]
    pos += 4
    stop, out = pos + size, {}
    while pos < stop:
        out[struct.unpack_from("<H", buf, pos)[0]] = struct.unpack_from("<Q", buf, pos + 2)[0]
        pos += 10
    return out, stop


class McapReader:
    """Parses an MCAP file: data section, chunks, summary section, footer."""

    def __init__(self, path=None, data=None):
        if data is None:
            with open(path, "rb") as handle:
                data = handle.read()
        self.buf = data
        self.header, self.statistics, self.footer = {}, {}, {}
        self.schemas, self.channels, self.metadata = {}, {}, {}
        self.messages, self.chunks = [], []
        self.chunk_indexes, self.summary_offsets = [], []
        self.metadata_indexes = []
        self.chunk_crc_ok, self.data_section_crc_ok = True, False
        self._parse()

    # -- helpers ------------------------------------------------------------
    def _parse(self):
        buf = self.buf
        if len(buf) < 2 * len(MAGIC) + 29 or buf[:8] != MAGIC or buf[-8:] != MAGIC:
            raise McapError("missing MCAP magic at start or end of file")
        footer_pos = len(buf) - 8 - 29
        opcode, body, _, _ = next(iter_records(buf, footer_pos, len(buf) - 8))
        if opcode != OP_FOOTER:
            raise McapError("last record before the trailing magic is not a Footer")
        summary_start, summary_offset_start, summary_crc = struct.unpack("<QQI", body)
        self.footer = {"summary_start": summary_start, "footer_offset": footer_pos,
                       "summary_offset_start": summary_offset_start,
                       "summary_crc": summary_crc}
        self._parse_data(8, footer_pos)
        if summary_start:
            expected = crc32c(buf[summary_start:summary_offset_start])
            self.footer["summary_crc_ok"] = expected == summary_crc
            self._parse_summary(summary_start, summary_offset_start, footer_pos)
        else:
            self.footer["summary_crc_ok"] = summary_crc == 0

    def _parse_data(self, start, end):
        for opcode, body, offset, _ in iter_records(self.buf, start, end):
            if opcode == OP_HEADER:
                self._parse_header(body)
            elif opcode == OP_SCHEMA:
                self._parse_schema(body)
            elif opcode == OP_CHANNEL:
                self._parse_channel(body)
            elif opcode == OP_MESSAGE:
                self.messages.append(self._decode_message(body))
            elif opcode == OP_CHUNK:
                self._parse_chunk(body)
            elif opcode == OP_METADATA:
                name, pos = decode_string(body, 0)
                self.metadata[name], _ = decode_string_map(body, pos)
            elif opcode == OP_DATA_END:
                stored = struct.unpack("<I", body)[0]
                # assumption 3: magic through DataEnd opcode+length
                self.data_section_crc_ok = stored == crc32c(self.buf[start:offset + 9])
                break

    def _parse_header(self, body):
        profile, pos = decode_string(body, 0)
        self.header = {"profile": profile, "library": decode_string(body, pos)[0]}

    def _parse_schema(self, body):
        schema_id = struct.unpack_from("<H", body, 0)[0]
        name, pos = decode_string(body, 2)
        encoding, pos = decode_string(body, pos)
        length = struct.unpack_from("<I", body, pos)[0]
        self.schemas[schema_id] = {"id": schema_id, "name": name,
                                   "encoding": encoding,
                                   "data": body[pos + 4:pos + 4 + length]}

    def _parse_channel(self, body):
        channel_id, schema_id = struct.unpack_from("<HH", body, 0)
        topic, pos = decode_string(body, 4)
        message_encoding, pos = decode_string(body, pos)
        metadata, _ = decode_string_map(body, pos)
        self.channels[channel_id] = {"id": channel_id, "schema_id": schema_id,
                                     "topic": topic, "metadata": metadata,
                                     "message_encoding": message_encoding}

    def _parse_chunk(self, body):
        start_time, end_time, size, crc = struct.unpack_from("<QQQI", body, 0)
        compression, pos = decode_string(body, 28)
        length = struct.unpack_from("<I", body, pos)[0]
        records = body[pos + 4:pos + 4 + length]
        if compression != "":
            raise McapError("unsupported compression %r" % compression)
        if len(records) != size or crc32c(records) != crc:
            self.chunk_crc_ok = False
        self.chunks.append(
            {"message_start_time": start_time, "message_end_time": end_time,
             "uncompressed_size": size, "uncompressed_crc": crc})
        for opcode, inner, _, _ in iter_records(records, 0, len(records)):
            if opcode == OP_MESSAGE:
                self.messages.append(self._decode_message(inner))

    @staticmethod
    def _decode_message(body):
        channel_id, sequence, log_time, publish_time = struct.unpack_from("<HIQQ", body, 0)
        length = struct.unpack_from("<I", body, 22)[0]
        return {"channel_id": channel_id, "sequence": sequence,
                "log_time": log_time, "publish_time": publish_time,
                "data": body[26:26 + length]}

    def _parse_summary(self, start, offset_start, end):
        for opcode, body, _, _ in iter_records(self.buf, start, offset_start):
            if opcode == OP_SCHEMA:
                self._parse_schema(body)
            elif opcode == OP_CHANNEL:
                self._parse_channel(body)
            elif opcode == OP_STATISTICS:
                self._parse_statistics(body)
            elif opcode == OP_CHUNK_INDEX:
                self.chunk_indexes.append(self._decode_chunk_index(body))
            elif opcode == OP_METADATA_INDEX:
                offset, length = struct.unpack_from("<QQ", body, 0)
                self.metadata_indexes.append({"offset": offset, "length": length,
                                              "name": decode_string(body, 16)[0]})
        for opcode, body, _, _ in iter_records(self.buf, offset_start, end):
            if opcode == OP_SUMMARY_OFFSET:
                group, group_start, group_length = struct.unpack("<BQQ", body)
                self.summary_offsets.append({"group_opcode": group,
                                             "group_start": group_start,
                                             "group_length": group_length})

    def _parse_statistics(self, body):
        (count, schemas, channels, attachments, metadatas, chunks,
         start, end) = struct.unpack_from("<QHIIIIQQ", body, 0)
        counts, _ = decode_u16_map(body, 42)
        self.statistics = {"message_count": count, "schema_count": schemas,
                           "channel_count": channels, "chunk_count": chunks,
                           "attachment_count": attachments,
                           "metadata_count": metadatas,
                           "message_start_time": start, "message_end_time": end,
                           "channel_message_counts": counts}

    @staticmethod
    def _decode_chunk_index(body):
        start_time, end_time, chunk_start, chunk_length = struct.unpack_from("<QQQQ", body, 0)
        offsets, pos = decode_u16_map(body, 32)
        message_index_length = struct.unpack_from("<Q", body, pos)[0]
        compression, pos = decode_string(body, pos + 8)
        compressed_size, uncompressed_size = struct.unpack_from("<QQ", body, pos)
        return {"message_start_time": start_time, "message_end_time": end_time,
                "chunk_start_offset": chunk_start, "chunk_length": chunk_length,
                "message_index_offsets": offsets, "compression": compression,
                "message_index_length": message_index_length,
                "compressed_size": compressed_size,
                "uncompressed_size": uncompressed_size}

    # -- message index walk -------------------------------------------------
    def chunk_records_start(self, chunk_start_offset):
        """File offset of a chunk's `records` field (assumption 1)."""
        opcode, _ = struct.unpack_from("<BQ", self.buf, chunk_start_offset)
        if opcode != OP_CHUNK:
            raise McapError("no Chunk record at %d" % chunk_start_offset)
        _, pos = decode_string(self.buf, chunk_start_offset + 9 + 28)
        return pos + 4

    def message_via_index(self, chunk_index, channel_id, entry_index):
        """Read one message through its MessageIndex entry (assumption 1)."""
        offset = chunk_index["message_index_offsets"][channel_id]
        _, body_len = struct.unpack_from("<BQ", self.buf, offset)
        channel = struct.unpack_from("<H", self.buf, offset + 9)[0]
        count = (body_len - 2) // 16
        if channel != channel_id or entry_index >= count:
            raise McapError("bad MessageIndex record at %d" % offset)
        log_time, message_offset = struct.unpack_from(
            "<QQ", self.buf, offset + 9 + 2 + 16 * entry_index)
        start = self.chunk_records_start(chunk_index["chunk_start_offset"]) + message_offset
        opcode, length = struct.unpack_from("<BQ", self.buf, start)
        if opcode != OP_MESSAGE:
            raise McapError("indexed offset %d does not point at a Message" % start)
        message = self._decode_message(self.buf[start + 9:start + 9 + length])
        if message["log_time"] != log_time:
            raise McapError("indexed log_time mismatch at %d" % start)
        return message

    def messages_by_topic(self):
        """Group decoded messages by topic name."""
        by_id = {cid: info["topic"] for cid, info in self.channels.items()}
        out = {}
        for message in self.messages:
            out.setdefault(by_id.get(message["channel_id"], "?"), []).append(message)
        return out
