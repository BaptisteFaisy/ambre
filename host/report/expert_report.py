#!/usr/bin/env python3
"""expert_report.py — Expert reading report of one Ambre event (spec 12.12).

Reads a raw journal image (--image) or an MCAP export (--mcap) and produces a
STANDALONE HTML report (one file, inline CSS, zero network dependency) in the
format of an expert reading report, with the regulation data-element table of
cahier des charges 6.1 filled with the values read from the event.

Decoders are REUSED, never duplicated:
  * raw image  -> host/mcap/ambre_journal_reader.JournalReader
  * MCAP file  -> host/mcap/ambre_mcap_writer.McapReader
  * class A point normalisation -> host/mcap/export_mcap.class_a_payload

Privacy (VIE-01/VIE-02): the tool scans its own output before writing it and
refuses to emit a document containing a vehicle identifier token, location
data or a calendar date. Only relative times are rendered.

Usage:
    python3 host/report/expert_report.py --image journal.img [--event-id N]
            --out report.html [--json report.json]
    python3 host/report/expert_report.py --mcap event.mcap --out report.html

Exit code: 0 when the report passed its own compliance and completeness
checks, otherwise the number of problems found.
"""

import argparse
import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
for sub in (("host", "mcap"), ("host", "report")):
    sys.path.insert(0, os.path.join(ROOT, *sub))

from ambre_journal_reader import JournalReader, crc32c        # noqa: E402
from ambre_mcap_writer import McapReader                      # noqa: E402
from export_mcap import class_a_payload, event_summary_payload  # noqa: E402
import report_html                                            # noqa: E402

# UN R160 5.3.1/5.3.2 trigger reasons (fmt/ambre_types.h, ambre_trig_reason_t).
TRIG_REASON_NAMES = {
    0: "none", 1: "delta-v longitudinal above 8 km/h within 150 ms",
    2: "delta-v lateral above 8 km/h within 150 ms",
    3: "non-reversible restraint deployed",
    4: "delta-v above 25 km/h within 150 ms (locked)",
    5: "vulnerable road user protection",
    6: "emergency braking (off-profile, never locked)",
    7: "automatic emergency braking (off-profile, never locked)",
    8: "service command (off-profile, never locked)",
}

# Tokens that must NEVER appear in the report (VIE-01): vehicle identifier,
# location data, calendar date. Shared with tests/test_report_py.py.
FORBIDDEN_PATTERNS = [
    (r"\bVIN\b", "vehicle identification number"),
    (r"(?i)\bgps\b|\blatitude\b|\blongitude\b|\bposition\b|\baddress\b",
     "location data"),
    (r"\b\d{4}-\d{2}-\d{2}\b|\b\d{2}/\d{2}/\d{4}\b|\b\d{1,2}\s+(?:jan|feb|mar|"
     r"apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\s+\d{4}\b", "calendar date"),
]


def sha256_of_file(path):
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def _event_payload_crc(view):
    """Chained CRC-32C over the event's record payloads (as the exporter)."""
    value = 0
    for record in view.records:
        value = crc32c(record["payload"], value)
    return "%08x" % value


def _journal_facts(reader):
    """Whole-journal figures: event count and time between events 1 and 2."""
    events = sorted(reader.events(), key=lambda ev: ev["trig_ts_us"])
    between = None
    if len(events) >= 2:
        between = (events[1]["trig_ts_us"] - events[0]["trig_ts_us"]) // 1000
    table, _ = reader.table()
    return {"event_count": len(events),
            "event_ids": [ev["event_id"] for ev in events],
            "time_between_events_ms": between,
            "session_id": table["session_id"] if table else None,
            "table_generation": table["generation"] if table else None}


def _event_dict(summary, sealed=None, ignition_cycle_at_read=None,
                t0_ts_us=0, trig_ts_us=0,
                max_dv_long_at_ms=0, max_dv_lat_at_ms=0):
    """Normalise one event summary (same keys in image and MCAP modes)."""
    trig = summary["trig_reason"]
    if sealed is None:
        sealed = summary["seal_cause"] != "none"
    return {
        "event_id": summary["event_id"],
        "trig_reason": trig,
        "trig_reason_name": TRIG_REASON_NAMES.get(trig, "unknown"),
        "locked": bool(summary["locked"]),
        "file_complete": bool(summary["file_complete"]),
        "sealed": sealed,
        "seal_cause": summary["seal_cause"],
        "event_number": summary["event_number"],
        "ignition_cycle": summary["ignition_cycle"],
        "ignition_cycle_at_read": ignition_cycle_at_read,
        "t0_ts_us": t0_ts_us,
        "trig_ts_us": trig_ts_us,
        "time_since_first_ms": summary["time_since_first_ms"],
        "max_dv_long_mm_s": summary["max_dv_long_mm_s"],
        "max_dv_long_at_ms": max_dv_long_at_ms,
        "max_dv_lat_mm_s": summary["max_dv_lat_mm_s"],
        "max_dv_lat_at_ms": max_dv_lat_at_ms,
        "restraint_deploy_ms": summary["restraint_deploy_ms"],
        "vru_deploy_ms": summary["vru_deploy_ms"],
    }


def facts_from_image(path, event_id=None):
    """Collect the report facts from a raw journal image."""
    reader = JournalReader(path)
    if not reader.events():
        raise ValueError("no event in the allocation table of %s" % path)
    if event_id is None:
        event_id = reader.first_event()["event_id"]
    view = reader.read_event(event_id)
    summary = event_summary_payload(view)
    seal = view.seal or {}
    emerg = view.emerg or {}
    by_signal = {}
    for _, samples in view.data:
        for sample in samples:
            by_signal[sample["signal_id"]] = \
                by_signal.get(sample["signal_id"], 0) + 1
    digest, size = sha256_of_file(path)
    return {
        "source": {"kind": "image", "path": path, "sha256": digest,
                   "bytes": size},
        "event": _event_dict(
            summary, sealed=view.sealed,
            ignition_cycle_at_read=seal.get("ignition_cycle_at_read"),
            t0_ts_us=view.t0_ts_us,
            trig_ts_us=emerg.get("trig_ts_us", view.desc["trig_ts_us"]),
            max_dv_long_at_ms=seal.get("max_dv_long_at_ms", 0),
            max_dv_lat_at_ms=seal.get("max_dv_lat_at_ms", 0)),
        "precrash": [class_a_payload(point) for point in view.precrash_points()],
        "delta_v": [{"rel_time_ms": cont["rel_time_ms"],
                     "dv_long_mm_s": cont["dv_long_mm_s"],
                     "dv_lat_mm_s": cont["dv_lat_mm_s"]}
                    for _, cont in view.cont_records()],
        "class_b": {"by_signal": by_signal, "messages": {},
                    "total": view.sample_count()},
        "gaps": view.gaps,
        "journal": _journal_facts(reader),
        "integrity": {"image_sha256": digest,
                      "event_payload_crc32c": _event_payload_crc(view),
                      "signature": "none"},
    }


def _json_messages(by_topic, topic):
    return [json.loads(message["data"].decode("utf-8"))
            for message in by_topic.get(topic, [])]


def facts_from_mcap(path):
    """Collect the report facts from an MCAP export of one event."""
    reader = McapReader(path)
    by_topic = reader.messages_by_topic()
    events = _json_messages(by_topic, "/edr/a/event")
    if not events:
        raise ValueError("no /edr/a/event message in %s" % path)
    summary = events[0]
    meta_event = reader.metadata.get("edr.event", {})
    integrity_meta = reader.metadata.get("edr.integrity", {})
    digest, size = sha256_of_file(path)
    messages = {topic: len(items) for topic, items in by_topic.items()}
    return {
        "source": {"kind": "mcap", "path": path, "sha256": digest,
                   "bytes": size},
        "event": _event_dict(
            summary,
            t0_ts_us=int(meta_event.get("t0_ts_us", 0)),
            trig_ts_us=int(integrity_meta.get("trig_ts_us", 0))),
        "precrash": _json_messages(by_topic, "/edr/a/precrash"),
        "delta_v": _json_messages(by_topic, "/edr/a/delta_v"),
        "class_b": {"by_signal": {},
                    "messages": {topic: count for topic, count in messages.items()
                                 if topic.startswith("/edr/b/")},
                    "total": sum(count for topic, count in messages.items()
                                 if topic.startswith("/edr/b/"))},
        "gaps": _json_messages(by_topic, "/edr/meta/gaps"),
        "journal": {"event_count": 1, "event_ids": [summary["event_id"]],
                    "time_between_events_ms": None, "session_id": None,
                    "table_generation": None},
        "integrity": {
            "image_sha256": integrity_meta.get("image_sha256") or None,
            "event_payload_crc32c": integrity_meta.get(
                "event_payload_crc32c", "unknown"),
            "signature": integrity_meta.get("signature", "none")},
    }


def self_check(html_text):
    """Return the list of problems: forbidden tokens, missing sections."""
    problems = []
    for pattern, label in FORBIDDEN_PATTERNS:
        if re.search(pattern, html_text):
            problems.append("forbidden data (%s) matched %r" % (label, pattern))
    for required in report_html.REQUIRED_IDS:
        if 'id="%s"' % required not in html_text:
            problems.append("missing required element id=%s" % required)
    return problems


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--image", help="raw journal image")
    source.add_argument("--mcap", help="MCAP export of one event")
    parser.add_argument("--event-id", type=int, default=None,
                        help="event to read (image mode; default: first)")
    parser.add_argument("--out", required=True, help="output HTML file")
    parser.add_argument("--json", default=None, help="also write the facts JSON")
    args = parser.parse_args(argv)

    if args.image:
        facts = facts_from_image(args.image, args.event_id)
    else:
        facts = facts_from_mcap(args.mcap)
    html_text = report_html.render_report(facts)
    problems = self_check(html_text)
    for problem in problems:
        print("expert-report: PROBLEM %s" % problem, file=sys.stderr)
    if problems:
        return len(problems)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(html_text)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(facts, handle, indent=2, sort_keys=True, default=str)
            handle.write("\n")
    event = facts["event"]
    print("expert-report: event %d (%s), locked=%s, file_complete=%s, "
          "%d class A points, %d delta-v points -> %s"
          % (event["event_id"], event["trig_reason_name"], event["locked"],
             event["file_complete"], len(facts["precrash"]),
             len(facts["delta_v"]), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
