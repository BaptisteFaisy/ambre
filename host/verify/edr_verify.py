"""edr-verify — integrity and fidelity check of an Ambre MCAP export (EXP-07).

CLI (docs/AGENT-BRIEF.md):

    python3 host/verify/edr_verify.py --mcap event.mcap \
            --scenario scenario.txt [--report out.json]

Checks, one line each in the report:

  mcap.structure      summary section, chunk indexes, summary offsets, footer,
                      chunk CRC, data-section CRC and summary CRC are present
  mcap.statistics     the Statistics record agrees with the decoded file
  mcap.channels       every message uses a known channel and a known schema,
                      and the channel set is exactly the one of FORMAT.md §12
  timestamps.monotonic  log times never go backwards inside a channel
  timestamps.t0       the single /edr/a/event message sits exactly at 60 s
  timestamps.relative no absolute timestamp leaks: every log time is a
                      microsecond multiple inside the 0..120 s window
  class_a.fidelity    the class A points of /edr/a/precrash equal the injected
                      scenario at the same instant, within the documented
                      quantisation tolerance
  metadata.required   edr.event, edr.vehicle_type, edr.build, edr.integrity
                      are present with their mandatory keys
  event.file_complete file_complete agrees with the metadata and with the seal
                      cause (complete <=> sealed normally)

Exit status: 0 when every check passes, 1 otherwise, 2 on a usage error.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "mcap"))
from ambre_mcap_writer import McapError, McapReader             # noqa: E402

T0_NS = 60 * 1_000_000_000          # T0 is placed at 60 s (FORMAT.md §12)
MAX_NS = 120 * 1_000_000_000        # no export may reach past 120 s
NS_PER_US = 1000
# A monotonic uptime stays far below this; a wall-clock epoch in microseconds is
# around 1.8e15, so anything above 1e15 us can only be an absolute timestamp.
ABSOLUTE_TIME_THRESHOLD_US = 10 ** 15

TOPIC_PRECRASH = "/edr/a/precrash"
TOPIC_DELTA_V = "/edr/a/delta_v"
TOPIC_EVENT = "/edr/a/event"
TOPIC_ACCEL = "/edr/b/accel"
TOPIC_VEHICLE = "/edr/b/vehicle"
TOPIC_STATUS = "/edr/b/status"
TOPIC_GAPS = "/edr/meta/gaps"

REQUIRED_TOPICS = {TOPIC_PRECRASH, TOPIC_DELTA_V, TOPIC_EVENT, TOPIC_ACCEL,
                   TOPIC_VEHICLE, TOPIC_STATUS}
KNOWN_TOPICS = REQUIRED_TOPICS | {TOPIC_GAPS}

REQUIRED_METADATA = {
    "edr.event": ("event_id", "trig_reason", "locked", "file_complete",
                  "ignition_cycle", "event_number"),
    "edr.vehicle_type": ("type", "variant", "version"),
    "edr.build": ("version", "profile", "config_sha256"),
    "edr.integrity": ("image_sha256", "image_bytes", "event_payload_crc32c",
                      "t0_offset_ns", "signature"),
}

# Quantisation tolerance, documented: each class A field is stored quantised in
# the journal (0.25 km/h, 1 %, 1 rpm, 0.5 deg, 1 mm/s2, 0.1 deg/s) and the
# scenario carries the same integers. We therefore allow one least significant
# unit of the stored field, which absorbs producer rounding without hiding a
# wrong sample: (payload field, scenario field, scale, tolerance).
CLASS_A_COMPARE = (
    ("speed_kmh", "speed_kmh_x4", 0.25, 0.25),
    ("throttle_pct", "throttle", 1.0, 1.0),
    ("brake", "brake", 1.0, 0.0),
    ("engine_rpm", "rpm", 1.0, 1.0),
    ("steer_deg", "steer_x2", 0.5, 0.5),
    ("accel_long_mm_s2", "a_long_mm_s2", 1.0, 1.0),
    ("accel_lat_mm_s2", "a_lat_mm_s2", 1.0, 1.0),
)

SCENARIO_FIELDS = ("t_us", "a_long_mm_s2", "a_lat_mm_s2", "speed_kmh_x4",
                   "throttle", "brake", "rpm", "steer_x2", "ext_flags")


class Report:
    """Collects (name, ok, detail) checks and renders them."""

    def __init__(self):
        self.checks = []

    def add(self, name, ok, detail=""):
        self.checks.append({"check": name, "ok": bool(ok), "detail": detail})
        return ok

    def ok(self):
        return all(check["ok"] for check in self.checks)

    def failures(self):
        return [check for check in self.checks if not check["ok"]]

    def render(self):
        lines = []
        for check in self.checks:
            lines.append("%s %-22s %s" % ("PASS" if check["ok"] else "FAIL",
                                          check["check"], check["detail"]))
        lines.append("%s: %d check(s), %d failure(s)"
                     % ("OK" if self.ok() else "KO", len(self.checks),
                        len(self.failures())))
        return "\n".join(lines)

    def to_dict(self, mcap_path, scenario_path):
        return {"mcap": mcap_path, "scenario": scenario_path,
                "ok": self.ok(),
                "checks": self.checks,
                "failures": len(self.failures())}


def load_scenario(path):
    """Parse a scenario file (FORMAT.md §11): header map + samples by t_us."""
    header, rows = {}, {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line.split()[0]:
                key, value = line.split("=", 1)
                header[key.strip()] = value.strip()
                continue
            values = [int(item) for item in line.split()]
            row = dict(zip(SCENARIO_FIELDS, values))
            rows[row["t_us"]] = row
    return header, rows


def messages_by_topic(reader):
    return reader.messages_by_topic()


def check_structure(report, reader):
    footer = reader.footer
    summary_ok = bool(footer.get("summary_start")) and bool(footer.get("summary_crc_ok"))
    report.add("mcap.structure",
               summary_ok and bool(reader.chunk_indexes)
               and bool(reader.summary_offsets) and reader.chunk_crc_ok
               and reader.data_section_crc_ok,
               "summary_start=%s summary_crc_ok=%s chunk_crc_ok=%s "
               "data_crc_ok=%s chunk_indexes=%d summary_offsets=%d"
               % (footer.get("summary_start"), footer.get("summary_crc_ok"),
                  reader.chunk_crc_ok, reader.data_section_crc_ok,
                  len(reader.chunk_indexes), len(reader.summary_offsets)))
    stats = reader.statistics
    report.add("mcap.statistics",
               stats.get("message_count") == len(reader.messages)
               and stats.get("chunk_count") == len(reader.chunks)
               and stats.get("channel_count") == len(reader.channels)
               and stats.get("schema_count") == len(reader.schemas),
               "messages=%s/%d chunks=%s/%d channels=%s/%d schemas=%s/%d"
               % (stats.get("message_count"), len(reader.messages),
                  stats.get("chunk_count"), len(reader.chunks),
                  stats.get("channel_count"), len(reader.channels),
                  stats.get("schema_count"), len(reader.schemas)))


def check_channels(report, reader):
    unknown = sorted({message["channel_id"] for message in reader.messages}
                     - set(reader.channels))
    no_schema = sorted(channel["topic"] for channel in reader.channels.values()
                       if channel["schema_id"] not in reader.schemas)
    topics = {channel["topic"] for channel in reader.channels.values()}
    missing = sorted(REQUIRED_TOPICS - topics)
    extra = sorted(topics - KNOWN_TOPICS)
    encodings = sorted({channel["message_encoding"]
                        for channel in reader.channels.values()})
    report.add("mcap.channels",
               not unknown and not no_schema and not missing and not extra
               and encodings == ["json"],
               "unknown_channels=%s without_schema=%s missing=%s extra=%s "
               "encodings=%s" % (unknown, no_schema, missing, extra, encodings))


def check_monotonic(report, reader):
    last, violation = {}, None
    for message in reader.messages:
        previous = last.get(message["channel_id"])
        if previous is not None and message["log_time"] < previous:
            violation = (message["channel_id"], previous, message["log_time"])
            break
        last[message["channel_id"]] = message["log_time"]
    report.add("timestamps.monotonic", violation is None,
               "non-decreasing per channel" if violation is None
               else "channel %d goes back from %d to %d" % violation)


def check_t0(report, by_topic):
    events = by_topic.get(TOPIC_EVENT, [])
    ok = len(events) == 1 and events[0]["log_time"] == T0_NS
    report.add("timestamps.t0", ok,
               "%d event message(s), log_time=%s (expected exactly one at %d)"
               % (len(events), events[0]["log_time"] if events else "-", T0_NS))


def check_relative_time(report, reader):
    bad = [message["log_time"] for message in reader.messages
           if message["log_time"] % NS_PER_US != 0
           or not 0 <= message["log_time"] <= MAX_NS]
    report.add("timestamps.relative", not bad,
               "%d message(s) outside 0..%d ns or not a microsecond multiple%s"
               % (len(bad), MAX_NS,
                  "" if not bad else " (first: %s)" % bad[0]))


def _integer_values(node):
    """Yield every integer found in a decoded JSON value."""
    if isinstance(node, bool):
        return
    if isinstance(node, int):
        yield node
    elif isinstance(node, dict):
        for value in node.values():
            yield from _integer_values(value)
    elif isinstance(node, list):
        for value in node:
            yield from _integer_values(value)


def check_no_absolute_time(report, reader):
    """VIE-02/EXP-04: no absolute timestamp may appear in payloads or metadata."""
    leaks = []
    for message in reader.messages:
        try:
            payload = json.loads(message["data"].decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            continue
        for value in _integer_values(payload):
            if value >= ABSOLUTE_TIME_THRESHOLD_US:
                leaks.append(("payload", value))
    for name, mapping in reader.metadata.items():
        for key, value in mapping.items():
            if value.isdigit() and int(value) >= ABSOLUTE_TIME_THRESHOLD_US:
                leaks.append(("%s.%s" % (name, key), int(value)))
    report.add("timestamps.no_absolute", not leaks,
               "%d value(s) >= %d us (an absolute epoch would be), e.g. %s"
               % (len(leaks), ABSOLUTE_TIME_THRESHOLD_US, leaks[:3]))


def _compare_point(payload, row):
    problems = []
    for field, scenario_field, scale, tolerance in CLASS_A_COMPARE:
        if field not in payload or scenario_field not in row:
            problems.append("missing field %s" % field)
            continue
        got = float(payload[field])
        want = float(row[scenario_field]) * scale
        if abs(got - want) > tolerance:
            problems.append("%s=%s expected %s" % (field, got, want))
    return problems


def _nearest_scenario_ms(rows, t_us):
    """Snap t_us to the nearest millisecond present in the scenario grid."""
    if t_us in rows:
        return t_us
    return int(round(t_us / 1000.0)) * 1000


def check_class_a(report, by_topic, scenario, reader):
    """Compare against the EVENT T0 (journal trigger time) read from the
    edr.event metadata, not the scenario crash_at_ms: the two differ by the
    trigger latency, and rel_time_ms is the real age of each point."""
    _, rows = scenario
    event_meta = reader.metadata.get("edr.event", {})
    raw_t0 = event_meta.get("t0_ts_us")
    if raw_t0 is None or not str(raw_t0).isdigit():
        report.add("class_a.fidelity", False,
                   "edr.event metadata carries no t0_ts_us: cannot map the "
                   "class A points onto the scenario")
        return
    t0_us = int(raw_t0)
    points = by_topic.get(TOPIC_PRECRASH, [])
    problems, checked = [], 0
    for message in points:
        payload = json.loads(message["data"].decode("utf-8"))
        t_us = _nearest_scenario_ms(rows, t0_us - payload["rel_time_ms"] * 1000)
        row = rows.get(t_us)
        if row is None:
            problems.append("no scenario sample at t_us=%d" % t_us)
            continue
        problems.extend(_compare_point(payload, row))
        checked += 1
    report.add("class_a.fidelity", bool(points) and not problems,
               "%d point(s) compared at T0=%d us, tolerance 1 LSB per field%s"
               % (checked, t0_us,
                  "" if not problems else "; " + "; ".join(problems[:5])))


def check_metadata(report, reader):
    missing_names = [name for name in REQUIRED_METADATA
                     if name not in reader.metadata]
    missing_keys = ["%s.%s" % (name, key)
                    for name, keys in REQUIRED_METADATA.items()
                    for key in keys
                    if key not in reader.metadata.get(name, {})]
    report.add("metadata.required",
               not missing_names and not missing_keys,
               "records=%d missing=%s missing_keys=%s"
               % (len(reader.metadata), missing_names, missing_keys))


def check_file_complete(report, reader, by_topic):
    events = by_topic.get(TOPIC_EVENT, [])
    if len(events) != 1:
        report.add("event.file_complete", False, "no unique event summary")
        return
    payload = json.loads(events[0]["data"].decode("utf-8"))
    metadata = reader.metadata.get("edr.event", {})
    declared = metadata.get("file_complete")
    complete = payload.get("file_complete")
    cause = payload.get("seal_cause")
    consistent = declared == str(complete)
    if complete == 1:
        # only a normal seal may declare the file complete (invariant I7)
        consistent = consistent and cause == "normal"
    else:
        # an incomplete file must never claim a normal seal; it is either
        # sealed by recovery (powercut/voltage/overwrite) or not sealed at all
        consistent = consistent and cause != "normal"
    consistent = consistent and metadata.get("locked") == str(payload.get("locked"))
    report.add("event.file_complete", consistent,
               "metadata=%s payload=%s seal_cause=%s locked=%s"
               % (declared, complete, cause, metadata.get("locked")))


def verify(mcap_path, scenario_path):
    """Run every check; returns (Report, reader)."""
    report = Report()
    reader = McapReader(mcap_path)
    by_topic = messages_by_topic(reader)
    scenario = load_scenario(scenario_path)
    check_structure(report, reader)
    check_channels(report, reader)
    check_monotonic(report, reader)
    check_t0(report, by_topic)
    check_relative_time(report, reader)
    check_no_absolute_time(report, reader)
    check_class_a(report, by_topic, scenario, reader)
    check_metadata(report, reader)
    check_file_complete(report, reader, by_topic)
    return report, reader


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Verify an Ambre MCAP export against its scenario")
    parser.add_argument("--mcap", required=True, help="exported .mcap file")
    parser.add_argument("--scenario", required=True, help="injected scenario")
    parser.add_argument("--report", help="write a JSON report to this path")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        report, _ = verify(args.mcap, args.scenario)
    except (OSError, McapError, KeyError, ValueError) as error:
        print("KO: %s: %s" % (type(error).__name__, error), file=sys.stderr)
        return 1
    print(report.render())
    if args.report:
        with open(args.report, "w", encoding="utf-8") as handle:
            json.dump(report.to_dict(args.mcap, args.scenario), handle, indent=2)
            handle.write("\n")
        print("report written to %s" % args.report)
    return 0 if report.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
