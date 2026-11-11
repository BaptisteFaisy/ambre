"""Export one Ambre event from a raw journal image to a single MCAP file.

CLI (docs/AGENT-BRIEF.md):

    python3 host/mcap/export_mcap.py --image journal.img --out event.mcap \
            [--event-id N] [--list]

The mapping follows docs/FORMAT.md §12: seven channels, JSON encoding with a
JSON schema per channel, no compression, chunk CRC enabled, summary section and
statistics present.

Time: T0 is placed at 60 s inside the file (VIE-02, EXP-04). Every MCAP
timestamp is derived only from the monotonic microsecond values carried by the
journal records, so no absolute timestamp can leak into the export.
"""

import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ambre_journal_reader import (EV_STATE_NAMES, JournalReader,  # noqa: E402
                                  SEAL_CAUSE_NAMES, crc32c)

from ambre_mcap_writer import McapWriter                          # noqa: E402

T0_OFFSET_NS = 60 * 1_000_000_000        # T0 sits at 60 s in the file
NS_PER_US = 1000
EVENT_VERSION = "0.1.0"
PROFILE = "socle"

TOPIC_PRECRASH = "/edr/a/precrash"
TOPIC_DELTA_V = "/edr/a/delta_v"
TOPIC_EVENT = "/edr/a/event"
TOPIC_ACCEL = "/edr/b/accel"
TOPIC_VEHICLE = "/edr/b/vehicle"
TOPIC_STATUS = "/edr/b/status"
TOPIC_GAPS = "/edr/meta/gaps"

SCHEMA_OF_TOPIC = {
    TOPIC_PRECRASH: "ambre.ClassAPoint",
    TOPIC_DELTA_V: "ambre.DeltaV",
    TOPIC_EVENT: "ambre.EventSummary",
    TOPIC_ACCEL: "ambre.Accel",
    TOPIC_VEHICLE: "ambre.Vehicle",
    TOPIC_STATUS: "ambre.Status",
    TOPIC_GAPS: "ambre.Gap",
}

# Sample cadences (docs/FORMAT.md §5) used to time the class B samples of a
# DATA record: the record carries one first_ts_us and samples of several
# signals, so the i-th sample of a signal is placed at first_ts_us + i / rate.
SIGNAL_RATE_HZ = {1: 1000, 2: 1000, 3: 1000, 10: 100, 11: 100, 12: 100, 13: 100,
                  20: 100, 21: 100, 22: 100, 23: 100, 24: 100, 25: 100,
                  40: 10, 41: 10, 42: 10, 43: 10}
ACCEL_FIELDS = {1: "x_mm_s2", 2: "y_mm_s2", 3: "z_mm_s2"}
VEHICLE_FIELDS = {10: "wheel_fl_mm_s", 11: "wheel_fr_mm_s", 12: "wheel_rl_mm_s",
                  13: "wheel_rr_mm_s", 20: "steer_deg", 21: "throttle_pct",
                  22: "brake", 23: "yaw_rate_dps", 24: "roll_rate_dps",
                  25: "engine_rpm"}
STATUS_FIELDS = {40: "adas", 41: "belts", 42: "lamps", 43: "gear"}
SCALE = {20: 0.5, 21: 0.5, 23: 0.1, 24: 0.1}

SCHEMAS = {
    "ambre.ClassAPoint": {
        "type": "object",
        "properties": {
            "rel_time_ms": {"type": "integer", "description": "ms before T0"},
            "speed_kmh": {"type": "number"},
            "throttle_pct": {"type": "integer"},
            "brake": {"type": "integer"},
            "engine_rpm": {"type": "integer"},
            "steer_deg": {"type": "number"},
            "accel_long_mm_s2": {"type": "integer"},
            "accel_lat_mm_s2": {"type": "integer"},
            "abs": {"type": "integer"}, "esc": {"type": "integer"},
            "belt_driver": {"type": "integer"},
            "airbag_lamp": {"type": "integer"},
            "yaw_rate_dps": {"type": "number"},
        },
        "required": ["rel_time_ms", "speed_kmh"],
    },
    "ambre.DeltaV": {
        "type": "object",
        "properties": {
            "rel_time_ms": {"type": "integer", "description": "ms after T0"},
            "dv_long_mm_s": {"type": "integer"},
            "dv_lat_mm_s": {"type": "integer"},
            "dv_long_kmh": {"type": "number"},
            "dv_lat_kmh": {"type": "number"},
        },
        "required": ["rel_time_ms", "dv_long_mm_s", "dv_lat_mm_s"],
    },
    "ambre.EventSummary": {
        "type": "object",
        "properties": {
            "event_id": {"type": "integer"}, "trig_reason": {"type": "integer"},
            "locked": {"type": "integer"}, "file_complete": {"type": "integer"},
            "event_number": {"type": "integer"},
            "ignition_cycle": {"type": "integer"},
            "seal_cause": {"type": "string"},
            "max_dv_long_mm_s": {"type": "integer"},
            "max_dv_lat_mm_s": {"type": "integer"},
            "restraint_deploy_ms": {"type": "integer"},
            "vru_deploy_ms": {"type": "integer"},
            "time_since_first_ms": {"type": "integer"},
            "cont_records": {"type": "integer"},
        },
        "required": ["event_id", "file_complete"],
    },
    "ambre.Accel": {
        "type": "object",
        "properties": {"x_mm_s2": {"type": "integer"},
                       "y_mm_s2": {"type": "integer"},
                       "z_mm_s2": {"type": "integer"},
                       "quality": {"type": "object"}},
    },
    "ambre.Vehicle": {
        "type": "object",
        "properties": {"wheel_fl_mm_s": {"type": "integer"},
                       "wheel_fr_mm_s": {"type": "integer"},
                       "wheel_rl_mm_s": {"type": "integer"},
                       "wheel_rr_mm_s": {"type": "integer"},
                       "steer_deg": {"type": "number"},
                       "throttle_pct": {"type": "number"},
                       "brake": {"type": "integer"},
                       "yaw_rate_dps": {"type": "number"},
                       "roll_rate_dps": {"type": "number"},
                       "engine_rpm": {"type": "integer"},
                       "quality": {"type": "object"}},
    },
    "ambre.Status": {
        "type": "object",
        "properties": {"adas": {"type": "integer"},
                       "belts": {"type": "integer"},
                       "lamps": {"type": "integer"},
                       "gear": {"type": "integer"},
                       "quality": {"type": "object"}},
    },
    "ambre.Gap": {
        "type": "object",
        "properties": {"start_ts_us": {"type": "integer"},
                       "end_ts_us": {"type": "integer"},
                       "duration_us": {"type": "integer"},
                       "reason": {"type": "integer"}},
        "required": ["start_ts_us", "end_ts_us"],
    },
}


def signal_target(signal_id):
    """(topic, field, scale) for a whitelisted signal id, else (None, None, 1)."""
    if signal_id in ACCEL_FIELDS:
        return TOPIC_ACCEL, ACCEL_FIELDS[signal_id], 1
    if signal_id in VEHICLE_FIELDS:
        return TOPIC_VEHICLE, VEHICLE_FIELDS[signal_id], SCALE.get(signal_id, 1)
    if signal_id in STATUS_FIELDS:
        return TOPIC_STATUS, STATUS_FIELDS[signal_id], 1
    return None, None, 1


def class_a_payload(point):
    """JSON body of one class A point (quantised values kept as published)."""
    return {
        "rel_time_ms": point["rel_time_ms"],
        "speed_kmh": point["speed_kmh_x4"] / 4.0,
        "throttle_pct": point["throttle_pct"],
        "brake": point["brake_active"],
        "engine_rpm": point["engine_rpm"],
        "steer_deg": point["steer_angle_deg_x2"] / 2.0,
        "accel_long_mm_s2": point["accel_long_mm_s2"],
        "accel_lat_mm_s2": point["accel_lat_mm_s2"],
        "abs": point["abs_active"], "esc": point["esc_active"],
        "belt_driver": point["belt_driver"],
        "airbag_lamp": point["airbag_lamp"],
        "yaw_rate_dps": point["yaw_rate_deg_s_x10"] / 10.0,
    }


def event_summary_payload(view):
    """JSON body of the single /edr/a/event message."""
    emerg = view.emerg or {}
    seal = view.seal or {}
    desc = view.desc
    return {
        "event_id": view.event_id,
        "trig_reason": emerg.get("trig_reason", desc["trig_reason"]),
        "locked": 1 if view.locked else 0,
        "file_complete": 1 if view.file_complete else 0,
        "event_number": emerg.get("event_number", 1),
        "ignition_cycle": emerg.get("ignition_cycle", 0),
        "seal_cause": SEAL_CAUSE_NAMES.get(seal.get("seal_cause", -1), "none"),
        "max_dv_long_mm_s": seal.get("max_dv_long_mm_s", 0),
        "max_dv_lat_mm_s": seal.get("max_dv_lat_mm_s", 0),
        "restraint_deploy_ms": seal.get("restraint_deploy_ms", 0),
        "vru_deploy_ms": seal.get("vru_deploy_ms", 0),
        "time_since_first_ms": seal.get("time_since_first_ms", 0),
        "cont_records": len(view.conts),
    }


class EventExporter:
    """Writes one EventView to one MCAP file."""

    def __init__(self, view, out_path, image_sha256="", image_bytes=0,
                 chunk_size=None):
        self.view = view
        self.out_path = out_path
        self.image_sha256 = image_sha256
        self.image_bytes = image_bytes
        self.t0_us = view.t0_ts_us
        self.writer = McapWriter(out_path, profile="ambre",
                                 library="ambre-host-mcap",
                                 chunk_size=chunk_size or 512 * 1024)
        self.channels = {}
        self.counts = {}
        for topic, schema_name in SCHEMA_OF_TOPIC.items():
            schema_id = self.writer.add_schema(
                schema_name, "jsonschema",
                json.dumps(SCHEMAS[schema_name], sort_keys=True).encode("utf-8"))
            self.channels[topic] = self.writer.add_channel(topic, "json", schema_id)

    # -- helpers ------------------------------------------------------------
    def ts_ns(self, ts_us):
        """Relative microseconds -> MCAP nanoseconds, T0 at 60 s (no absolute time)."""
        return max(0, T0_OFFSET_NS + (ts_us - self.t0_us) * NS_PER_US)

    def emit(self, topic, ts_us, payload):
        channel_id = self.channels[topic]
        body = json.dumps(payload, sort_keys=True).encode("utf-8")
        self.writer.add_message(channel_id, self.ts_ns(ts_us), body)
        self.counts[topic] = self.counts.get(topic, 0) + 1

    # -- channels -----------------------------------------------------------
    def _export_precrash(self):
        for point in self.view.precrash_points():
            self.emit(TOPIC_PRECRASH, self.t0_us - point["rel_time_ms"] * 1000,
                      class_a_payload(point))

    def _export_delta_v(self):
        for _, cont in self.view.cont_records():
            self.emit(TOPIC_DELTA_V, self.t0_us + cont["rel_time_ms"] * 1000,
                      {"rel_time_ms": cont["rel_time_ms"],
                       "dv_long_mm_s": cont["dv_long_mm_s"],
                       "dv_lat_mm_s": cont["dv_lat_mm_s"],
                       "dv_long_kmh": cont["dv_long_mm_s"] * 0.0036,
                       "dv_lat_kmh": cont["dv_lat_mm_s"] * 0.0036})

    def _export_event(self):
        self.emit(TOPIC_EVENT, self.t0_us, event_summary_payload(self.view))

    def _export_gaps(self):
        for gap in self.view.gaps:
            self.emit(TOPIC_GAPS, gap["start_ts_us"],
                      {"start_ts_us": gap["start_ts_us"],
                       "end_ts_us": gap["end_ts_us"],
                       "duration_us": gap["end_ts_us"] - gap["start_ts_us"],
                       "reason": gap["reason"]})

    def _class_b_messages(self):
        """(topic, ts_us, payload) for every class B sample of the event."""
        pending = {}
        for first_ts_us, samples in self.view.data:
            counters = {}
            for sample in samples:
                topic, field, scale = signal_target(sample["signal_id"])
                if topic is None:
                    continue
                signal_id = sample["signal_id"]
                index = counters.get(signal_id, 0)
                counters[signal_id] = index + 1
                step_us = 1_000_000 // SIGNAL_RATE_HZ[signal_id]
                ts_us = first_ts_us + index * step_us
                slot = pending.setdefault((topic, ts_us), {})
                slot[field] = sample["value"] * scale if scale != 1 else sample["value"]
                slot.setdefault("quality", {})[field] = sample["quality"]
        return [(topic, ts_us, payload)
                for (topic, ts_us), payload in sorted(pending.items())]

    def _export_class_b(self):
        for topic, ts_us, payload in self._class_b_messages():
            self.emit(topic, ts_us, payload)

    # -- metadata -----------------------------------------------------------
    def _metadata(self):
        summary = event_summary_payload(self.view)
        emerg = self.view.emerg or {}
        record_crc = 0
        for record in self.view.records:
            record_crc = crc32c(record["payload"], record_crc)
        return {
            "edr.event": {
                "event_id": str(summary["event_id"]),
                "trig_reason": str(summary["trig_reason"]),
                "locked": str(summary["locked"]),
                "file_complete": str(summary["file_complete"]),
                "ignition_cycle": str(summary["ignition_cycle"]),
                "event_number": str(summary["event_number"]),
                "seal_cause": summary["seal_cause"],
            },
            "edr.vehicle_type": {"type": "ambre-edr", "variant": "socle",
                                 "version": "1"},
            "edr.build": {"version": EVENT_VERSION, "profile": PROFILE,
                          "config_sha256": hashlib.sha256(
                              b"ambre-format-v1").hexdigest()},
            "edr.integrity": {
                "image_sha256": self.image_sha256,
                "image_bytes": str(self.image_bytes),
                "event_payload_crc32c": "%08x" % record_crc,
                "t0_offset_ns": str(T0_OFFSET_NS),
                "trig_ts_us": str(emerg.get("trig_ts_us", 0)),
                "signature": "none",
            },
        }

    # -- driver -------------------------------------------------------------
    def run(self):
        self._export_precrash()
        self._export_delta_v()
        self._export_event()
        self._export_class_b()
        self._export_gaps()
        for name, mapping in self._metadata().items():
            self.writer.add_metadata(name, mapping)
        self.writer.finish()
        return {"out": self.out_path, "counts": dict(self.counts),
                "messages": sum(self.counts.values())}


def image_digest(path):
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def export_event(reader, event_id, out_path, image_path=None, chunk_size=None):
    """Export one event of an open journal to `out_path`; returns a summary."""
    view = reader.read_event(event_id)
    sha, size = image_digest(image_path) if image_path else ("", 0)
    return EventExporter(view, out_path, sha, size, chunk_size).run()


def list_events(reader):
    """One line per event: id, state, locked, complete, T0, points, records.

    `t0_us` is the monotonic microsecond value carried by the journal (no
    absolute epoch); `t0_at_s` is where T0 lands in the MCAP file, always the
    fixed 60 s offset of FORMAT.md §12.
    """
    events = reader.events()
    if not events:
        print("no event in the allocation table")
        return 0
    print("%-6s %-7s %-8s %-9s %-14s %-9s %-7s %s"
          % ("id", "state", "locked", "complete", "t0_us", "t0_at_s",
             "points", "records"))
    for event in events:
        view = reader.read_event(event["event_id"])
        print("%-6d %-7s %-8s %-9s %-14d %-9.1f %-7d %d"
              % (event["event_id"], EV_STATE_NAMES.get(event["state"], "?"),
                 "yes" if view.locked else "no",
                 "yes" if view.file_complete else "no",
                 view.t0_ts_us, T0_OFFSET_NS / 1e9,
                 view.point_count, len(view.records)))
    return len(events)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Export one Ambre event from a journal image to MCAP")
    parser.add_argument("--image", required=True, help="raw journal image")
    parser.add_argument("--out", help="output .mcap file")
    parser.add_argument("--event-id", type=int, default=None,
                        help="event to export (default: first sealed/acquired)")
    parser.add_argument("--list", action="store_true",
                        help="list the events found in the image")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    reader = JournalReader(args.image)
    table, copy_index = reader.table()
    if table is None:
        print("KO: neither allocation table copy is valid", file=sys.stderr)
        return 2
    print("table copy %s, generation %d, session %d, %d event(s)"
          % ("A" if copy_index == 0 else "B", table["generation"],
             table["session_id"], len(reader.events())))
    if args.list:
        list_events(reader)
        return 0
    if not args.out:
        print("KO: --out is required unless --list is used", file=sys.stderr)
        return 2
    if args.event_id is None:
        chosen = reader.first_event()
        if chosen is None:
            print("KO: no event to export", file=sys.stderr)
            return 1
        event_id = chosen["event_id"]
    else:
        event_id = args.event_id
    result = export_event(reader, event_id, args.out, args.image)
    print("exported event %d to %s: %d message(s) %s"
          % (event_id, result["out"], result["messages"],
             json.dumps(result["counts"], sort_keys=True)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
