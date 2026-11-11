"""Autonomous tests for the Python MCAP restitution chain.

Run with:  python3 tests/test_mcap_py.py
Exit code: number of failing tests (0 = green).

Everything is fabricated in Python from docs/FORMAT.md: the journal image is
built block by block here (the C codec is not needed), then read back, exported
to MCAP and re-verified.
"""

import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "host", "mcap"))
sys.path.insert(0, os.path.join(ROOT, "host", "verify"))

from ambre_journal_reader import (                                    # noqa: E402
    A_POINT_KEYS, BLOCK_PAYLOAD_MAX, EV_SEALED, EV_OPEN, FIRST_DATA_OFFSET,
    FORMAT_VERSION, MAGIC_BLOCK, MAGIC_TABLE, MAX_EVENTS, REC_CONT, REC_DATA,
    REC_EMERG, REC_GAP, REC_SEAL, SEGMENT_SIZE, SEG_CURRENT, JournalReader,
    check_layout, crc32c, crc32c_available_accelerator)
import ambre_journal_reader as journal_reader                        # noqa: E402
import ambre_mcap_writer as mcap_writer                               # noqa: E402
import edr_verify                                                     # noqa: E402
import export_mcap                                                    # noqa: E402

T0_US = 25_000_000
CRASH_AT_MS = 25_000
SESSION = 0x1234


# ------------------------------------------------------- image construction --

def truth_at(t_us):
    """Ground truth of the injected scenario at monotonic time t_us."""
    return {"a_long_mm_s2": 1200, "a_lat_mm_s2": -300,
            "speed_kmh_x4": 200 + (t_us // 100_000) % 40,
            "throttle": 40, "brake": 0, "rpm": 1500, "steer_x2": 10,
            "ext_flags": 0}


def truth_point(t_us):
    """Class A point of the injected scenario at t_us."""
    truth = truth_at(t_us)
    return {"speed_kmh_x4": truth["speed_kmh_x4"],
            "throttle_pct": truth["throttle"], "brake_active": truth["brake"],
            "engine_rpm": truth["rpm"],
            "steer_angle_deg_x2": truth["steer_x2"],
            "accel_long_mm_s2": truth["a_long_mm_s2"],
            "accel_lat_mm_s2": truth["a_lat_mm_s2"],
            "abs_active": 0, "esc_active": 1, "belt_driver": 1,
            "airbag_lamp": 0, "yaw_rate_deg_s_x10": 15,
            "rel_time_ms": (T0_US - t_us) // 1000}


def pack_a_point(point):
    return struct.pack("<HBBHhhhBBBBhH", *[point[key] for key in A_POINT_KEYS])


def pack_block(rec_type, seq, first_ts_us, payload, chunk_index=0,
               chunk_count=1, total_len=None):
    if total_len is None:
        total_len = len(payload)
    header = struct.pack("<IHBBQQQHHIII", MAGIC_BLOCK, FORMAT_VERSION, rec_type,
                         0, seq, SESSION, first_ts_us, chunk_index,
                         chunk_count, len(payload), total_len, crc32c(payload))
    body = header + payload + b"\x00" * (BLOCK_PAYLOAD_MAX - len(payload))
    return body + struct.pack("<I", crc32c(body))


def pack_record(rec_type, seq, first_ts_us, payload):
    """One record, split over as many blocks as its payload needs."""
    total = len(payload)
    count = max(1, (total + BLOCK_PAYLOAD_MAX - 1) // BLOCK_PAYLOAD_MAX)
    return [pack_block(rec_type, seq, first_ts_us,
                       payload[index * BLOCK_PAYLOAD_MAX:(index + 1) * BLOCK_PAYLOAD_MAX],
                       index, count, total) for index in range(count)]


def pack_emerg(event_id, trig_reason, locked, t0_ts_us, points):
    payload = struct.pack("<IBBBBQQIHH", event_id, trig_reason, locked, 1, 2,
                          t0_ts_us, t0_ts_us, 1, len(points), 0)
    payload += b"".join(pack_a_point(point) for point in points)
    payload += b"\x00" * (21 - len(points)) * 20          # fixed-size C struct
    return payload


def pack_cont(event_id, rel_time_ms, dv_long, dv_lat, points):
    payload = struct.pack("<IHHii", event_id, len(points), rel_time_ms,
                          dv_long, dv_lat)
    payload += b"".join(pack_a_point(point) for point in points)
    return payload + b"\x00" * (4 - len(points)) * 20


def pack_seal(event_id, file_complete, seal_cause, locked, max_dv_long):
    return struct.pack("<IBBBBIiHiHHHI", event_id, file_complete, seal_cause,
                       locked, 1, 21_000, max_dv_long, 21_000, -900, 21_000,
                       0, 0, 2)


def pack_gap(start_ts_us, end_ts_us, reason):
    return struct.pack("<QQI", start_ts_us, end_ts_us, reason)


def pack_samples(samples):
    return b"".join(struct.pack("<HBBi", item["signal_id"], item["quality"],
                                0, item["value"]) for item in samples)


def pack_desc(event):
    return struct.pack("<IIBBBBIIIIQQQ", event["event_id"], event.get("order", 1),
                       event["state"], event["locked"], event["file_complete"],
                       event["trig_reason"], event["start_seg"],
                       event["start_block"], event["end_seg"],
                       event["end_block"], event["trig_ts_us"],
                       event["seq_emerg"], event["seq_last"])


def pack_table(generation, events, current_block):
    states = bytearray(24)
    states[0] = SEG_CURRENT
    body = struct.pack("<IIIIQQQIIII24s", MAGIC_TABLE, FORMAT_VERSION,
                       generation, 0, SESSION, 9, 3, 0, current_block,
                       len(events), 1, bytes(states))
    for event in events:
        body += pack_desc(event)
    empty = {"event_id": 0, "state": 0, "locked": 0, "file_complete": 0,
             "trig_reason": 0, "start_seg": 0, "start_block": 0, "end_seg": 0,
             "end_block": 0, "trig_ts_us": 0, "seq_emerg": 0, "seq_last": 0}
    for _ in range(MAX_EVENTS - len(events)):
        body += pack_desc(empty)
    body += b"\x00" * 12
    assert len(body) == 508, len(body)
    return body + struct.pack("<I", crc32c(body))


def data_samples(count=200):
    """Class B samples: accel x/y/z, one wheel speed and one status signal."""
    signals = (1, 2, 3, 10, 40)
    return [{"signal_id": signals[i % len(signals)], "quality": 0,
             "value": 1000 + i} for i in range(count)]


def build_image_bytes():
    """Synthetic journal image: one sealed event and one open event."""
    image = bytearray(FIRST_DATA_OFFSET + SEGMENT_SIZE)
    points = [truth_point(T0_US - rel * 1000) for rel in range(5000, -1, -500)]
    blocks, seq, blk = [], 1, 0

    def put(rec_type, payload, first_ts_us, count=1):
        nonlocal seq, blk
        packed = pack_record(rec_type, seq, first_ts_us, payload)
        for index, block in enumerate(packed):
            start = JournalReader.block_offset(0, blk + index)
            image[start:start + 512] = block
        blk += len(packed)
        seq += 1
        return count

    put(REC_EMERG, pack_emerg(1, 3, 1, T0_US, points), T0_US)
    for step in range(3):
        put(REC_CONT, pack_cont(1, step * 10, 500 * (step + 1), -100,
                                [truth_point(T0_US) for _ in range(4)]),
            T0_US + step * 10_000)
    put(REC_DATA, pack_samples(data_samples()), T0_US - 200_000)
    put(REC_GAP, pack_gap(T0_US + 5_000, T0_US + 8_000, 1), T0_US + 5_000)
    put(REC_SEAL, pack_seal(1, 1, 0, 1, 1200), T0_US + 21_000)
    put(REC_EMERG, pack_emerg(2, 1, 0, T0_US + 60_000, points), T0_US + 60_000)

    events = [
        {"event_id": 1, "order": 1, "state": EV_SEALED, "locked": 1,
         "file_complete": 1, "trig_reason": 3, "start_seg": 0, "start_block": 0,
         "end_seg": 0, "end_block": 9, "trig_ts_us": T0_US, "seq_emerg": 1,
         "seq_last": 7},
        {"event_id": 2, "order": 2, "state": EV_OPEN, "locked": 0,
         "file_complete": 0, "trig_reason": 1, "start_seg": 0, "start_block": 10,
         "end_seg": 0, "end_block": 10, "trig_ts_us": T0_US + 60_000,
         "seq_emerg": 8, "seq_last": 0},
    ]
    image[0:512] = pack_table(7, events, blk)              # copy A, newest
    image[512:1024] = pack_table(6, events, blk)           # copy B, older
    return bytes(image)


def write_scenario(path, duration_ms=30_000):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# ambre scenario v1\nname=unit-test\nseed=1\nrate_hz=1000\n")
        handle.write("duration_ms=%d\ncrash_at_ms=%d\n" % (duration_ms, CRASH_AT_MS))
        handle.write("# t_us a_long a_lat speed_x4 throttle brake rpm steer_x2 flags\n")
        for t_us in range(0, duration_ms * 1000 + 1, 1000):
            truth = truth_at(t_us)
            handle.write("%d %d %d %d %d %d %d %d %d\n" % (
                t_us, truth["a_long_mm_s2"], truth["a_lat_mm_s2"],
                truth["speed_kmh_x4"], truth["throttle"], truth["brake"],
                truth["rpm"], truth["steer_x2"], truth["ext_flags"]))


# ------------------------------------------------------------------- tests ---

class TestCrc32c(unittest.TestCase):
    def test_check_value(self):
        self.assertEqual(crc32c(b"123456789"), 0xE3069283)

    def test_known_vectors(self):
        self.assertEqual(crc32c(b""), 0)
        self.assertEqual(crc32c(b"a"), 0xC1D04330)
        self.assertEqual(crc32c(b"The quick brown fox jumps over the lazy dog"),
                         0x22620404)
        self.assertEqual(crc32c(b"\x00" * 32), 0x8A9136AA)

    def test_accelerator_agrees_with_pure_python(self):
        if not crc32c_available_accelerator():
            self.skipTest("no compiled crc32c module on this host")
        data = bytes(range(256)) * 3
        self.assertEqual(crc32c(data), journal_reader._crc32c_python(data))

    def test_incremental(self):
        data = bytes(range(256)) * 4
        whole = crc32c(data)
        self.assertEqual(crc32c(data[100:], crc32c(data[:100])), whole)

    def test_layout_sizes(self):
        sizes = check_layout()
        self.assertEqual(sizes["block header"], (48, 48))
        self.assertEqual(sizes["allocation table"], (512, 512))
        self.assertEqual(sizes["event descriptor"], (52, 52))
        self.assertEqual(sizes["class A point"], (20, 20))
        self.assertEqual(sizes["class B sample"], (8, 8))


class TestJournalReader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="ambre-test-")
        cls.image_path = os.path.join(cls.tmp, "journal.img")
        cls.image = build_image_bytes()
        with open(cls.image_path, "wb") as handle:
            handle.write(cls.image)

    def reader(self):
        return JournalReader(self.image_path)

    def test_table_copy_selection(self):
        table, copy_index = self.reader().table()
        self.assertEqual(copy_index, 0)
        self.assertEqual(table["generation"], 7)
        self.assertEqual(table["session_id"], SESSION)
        self.assertEqual(len(self.reader().events()), 2)

    def test_multichunk_reassembly(self):
        view = self.reader().read_event(1)
        self.assertEqual(view.sample_count(), 200)
        self.assertEqual(len(view.data), 1)
        first_ts, samples = view.data[0]
        self.assertEqual(first_ts, T0_US - 200_000)
        self.assertEqual(samples[0]["signal_id"], 1)
        self.assertEqual(samples[199]["value"], 1000 + 199)

    def test_emerg_cont_seal_fields(self):
        view = self.reader().read_event(1)
        self.assertIsNotNone(view.emerg)
        self.assertEqual(view.emerg["event_id"], 1)
        self.assertEqual(view.emerg["point_count"], 11)
        self.assertEqual(len(view.precrash_points()), 11)
        self.assertEqual(view.precrash_points()[0]["rel_time_ms"], 5000)
        self.assertEqual(view.precrash_points()[-1]["rel_time_ms"], 0)
        self.assertEqual(len(view.conts), 3)
        self.assertEqual(view.cont_records()[-1][1]["dv_long_mm_s"], 1500)
        self.assertIsNotNone(view.seal)
        self.assertEqual(view.seal["file_complete"], 1)
        self.assertEqual(view.seal["max_dv_long_mm_s"], 1200)
        self.assertEqual(len(view.gaps), 1)
        self.assertEqual(view.gaps[0]["reason"], 1)
        self.assertTrue(view.locked)
        self.assertTrue(view.file_complete)

    def test_open_event_is_not_sealed(self):
        view = self.reader().read_event(2)
        self.assertIsNone(view.seal)
        self.assertFalse(view.file_complete)
        self.assertEqual(view.desc["state"], EV_OPEN)

    def test_corrupt_block_is_dropped(self):
        corrupted = bytearray(self.image)
        start = JournalReader.block_offset(0, 6)       # 3rd chunk of the DATA
        corrupted[start + 100] ^= 0x01                 # flip one payload bit
        reader = JournalReader(data=bytes(corrupted))
        view = reader.read_event(1)
        self.assertEqual(view.sample_count(), 0)       # whole record dropped
        self.assertEqual(len(view.conts), 3)           # neighbours still valid
        self.assertIsNotNone(view.seal)

    def test_torn_multichunk_record_is_dropped(self):
        torn = bytearray(self.image)
        start = JournalReader.block_offset(0, 7)       # last chunk wiped
        torn[start:start + 512] = b"\x00" * 512
        reader = JournalReader(data=bytes(torn))
        self.assertEqual(reader.read_event(1).sample_count(), 0)
        self.assertEqual(len(reader.records()), 7)     # 8 records minus the DATA

    def test_bad_table_crc_falls_back_to_other_copy(self):
        broken = bytearray(self.image)
        broken[0:512] = b"\x00" * 512                  # copy A destroyed
        table, copy_index = JournalReader(data=bytes(broken)).table()
        self.assertEqual(copy_index, 1)
        self.assertEqual(table["generation"], 6)


class TestMcapExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="ambre-mcap-")
        cls.image_path = os.path.join(cls.tmp, "journal.img")
        cls.scenario_path = os.path.join(cls.tmp, "scenario.txt")
        cls.mcap_path = os.path.join(cls.tmp, "event.mcap")
        with open(cls.image_path, "wb") as handle:
            handle.write(build_image_bytes())
        write_scenario(cls.scenario_path)
        cls.reader = JournalReader(cls.image_path)
        cls.result = export_mcap.export_event(cls.reader, 1, cls.mcap_path,
                                              cls.image_path)

    def test_export_counts(self):
        counts = self.result["counts"]
        self.assertEqual(counts[export_mcap.TOPIC_PRECRASH], 11)
        self.assertEqual(counts[export_mcap.TOPIC_DELTA_V], 3)
        self.assertEqual(counts[export_mcap.TOPIC_EVENT], 1)
        self.assertEqual(counts[export_mcap.TOPIC_GAPS], 1)
        self.assertGreater(counts[export_mcap.TOPIC_ACCEL], 0)

    def test_mcap_structure(self):
        reader = mcap_writer.McapReader(self.mcap_path)
        self.assertTrue(reader.footer["summary_start"] > 0)
        self.assertTrue(reader.footer["summary_crc_ok"])
        self.assertTrue(reader.chunk_crc_ok)
        self.assertTrue(reader.data_section_crc_ok)
        self.assertTrue(reader.chunk_indexes)
        self.assertTrue(reader.summary_offsets)
        self.assertEqual(reader.statistics["message_count"],
                         len(reader.messages))
        topics = {channel["topic"] for channel in reader.channels.values()}
        self.assertEqual(topics, set(export_mcap.SCHEMA_OF_TOPIC))
        for channel in reader.channels.values():
            self.assertIn(channel["schema_id"], reader.schemas)
        # the message index really points at the messages it claims
        channel_id = reader.channels[1]["id"]
        index = reader.chunk_indexes[0]
        message = reader.message_via_index(index, channel_id, 0)
        self.assertEqual(message["log_time"] % 1000, 0)

    def test_t0_is_at_sixty_seconds(self):
        by_topic = mcap_writer.McapReader(self.mcap_path).messages_by_topic()
        events = by_topic[export_mcap.TOPIC_EVENT]
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["log_time"], 60 * 10 ** 9)
        self.assertLess(min(m["log_time"] for m in events), 61 * 10 ** 9)

    def test_verify_passes(self):
        report_path = os.path.join(self.tmp, "report.json")
        status = edr_verify.main(["--mcap", self.mcap_path, "--scenario",
                                  self.scenario_path, "--report", report_path])
        with open(report_path, encoding="utf-8") as handle:
            report = json.load(handle)
        self.assertEqual(status, 0, json.dumps(report, indent=2))
        self.assertTrue(report["ok"])
        self.assertEqual(report["failures"], 0)

    def test_verify_detects_a_wrong_scenario(self):
        other = os.path.join(self.tmp, "scenario-bad.txt")
        with open(other, "w", encoding="utf-8") as handle:
            handle.write("name=bad\ncrash_at_ms=%d\n" % CRASH_AT_MS)
            for t_us in range(0, 30_000_001, 1000):
                handle.write("%d 0 0 100 0 0 900 0 0\n" % t_us)
        status = edr_verify.main(["--mcap", self.mcap_path, "--scenario", other])
        self.assertEqual(status, 1)

    def test_multi_chunk_export_stays_valid(self):
        out = os.path.join(self.tmp, "chunked.mcap")
        export_mcap.export_event(self.reader, 1, out, self.image_path,
                                 chunk_size=2000)
        reader = mcap_writer.McapReader(out)
        self.assertGreater(len(reader.chunks), 1)
        self.assertEqual(len(reader.chunk_indexes), len(reader.chunks))
        self.assertTrue(reader.chunk_crc_ok and reader.footer["summary_crc_ok"])
        self.assertEqual(reader.statistics["message_count"], len(reader.messages))
        status = edr_verify.main(["--mcap", out, "--scenario", self.scenario_path])
        self.assertEqual(status, 0)

    def test_list_events(self):
        status = export_mcap.main(["--image", self.image_path, "--list"])
        self.assertEqual(status, 0)

    def test_default_event_is_the_first_sealed_one(self):
        chosen = self.reader.first_event()
        self.assertEqual(chosen["event_id"], 1)


def _mcap_lib_available():
    try:
        import mcap.reader                                    # noqa: F401,PLC0415
    except ImportError:
        return False
    return True


@unittest.skipUnless(_mcap_lib_available(), "mcap library not installed")
class TestAgainstMcapLibrary(unittest.TestCase):
    def test_message_count_matches(self):
        from mcap.reader import make_reader                    # noqa: PLC0415
        tmp = tempfile.mkdtemp(prefix="ambre-lib-")
        image_path = os.path.join(tmp, "journal.img")
        out = os.path.join(tmp, "event.mcap")
        with open(image_path, "wb") as handle:
            handle.write(build_image_bytes())
        export_mcap.export_event(JournalReader(image_path), 1, out, image_path)
        with open(out, "rb") as handle:
            summary = make_reader(handle).get_summary()
        count = sum(channel.message_count
                    for channel in summary.statistics.channel_message_counts.values()) \
            if summary.statistics else 0
        ours = len(mcap_writer.McapReader(out).messages)
        self.assertEqual(count, ours)


def main():
    suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return len(result.failures) + len(result.errors)


if __name__ == "__main__":
    sys.exit(main())
