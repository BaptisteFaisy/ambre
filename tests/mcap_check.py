#!/usr/bin/env python3
"""Cross-check a C++ MCAP archive against the host Python reader and verifier.

Reuses host/mcap/ambre_mcap_writer.py (McapReader), host/mcap/export_mcap.py
and host/verify/edr_verify.py without re-implementing any decoding or check.
"""

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "host", "mcap"))
sys.path.insert(0, os.path.join(ROOT, "host", "verify"))

from ambre_mcap_writer import McapReader                              # noqa: E402
import edr_verify                                                     # noqa: E402
import export_mcap                                                    # noqa: E402


def check_schemas_and_indexes(reader, expect_points):
    """Extra checks: exact schema JSON bodies, MessageIndex walk, gaps."""
    for channel in reader.channels.values():
        topic = channel["topic"]
        schema = reader.schemas[channel["schema_id"]]
        expected_name = export_mcap.SCHEMA_OF_TOPIC[topic]
        if schema["name"] != expected_name:
            raise AssertionError("topic %s schema %s != %s"
                                 % (topic, schema["name"], expected_name))
        if schema["encoding"] != "jsonschema":
            raise AssertionError("schema %s encoding %s"
                                 % (schema["name"], schema["encoding"]))
        got_schema = json.loads(schema["data"].decode("utf-8"))
        if got_schema != export_mcap.SCHEMAS[expected_name]:
            raise AssertionError("schema body mismatch for %s" % expected_name)
    for index in reader.chunk_indexes:
        for channel_id in index["message_index_offsets"]:
            msg = reader.message_via_index(index, channel_id, 0)
            if msg["channel_id"] != channel_id:
                raise AssertionError("indexed channel mismatch")
    by_topic = reader.messages_by_topic()
    if expect_points is not None:
        got = [json.loads(m["data"].decode("utf-8"))
               for m in by_topic.get(export_mcap.TOPIC_PRECRASH, [])]
        if len(got) != expect_points:
            raise AssertionError("expected %d class A points, got %d"
                                 % (expect_points, len(got)))


def compare_with_python_export(cpp_path, py_path):
    """Compare two MCAP exports (C++ target vs Python host) message by message."""
    cpp = McapReader(cpp_path)
    py = McapReader(py_path)
    cpp_topics = cpp.messages_by_topic()
    py_topics = py.messages_by_topic()
    if set(cpp_topics) != set(py_topics):
        raise AssertionError("topic sets differ: %s vs %s"
                             % (sorted(cpp_topics), sorted(py_topics)))
    for topic in sorted(py_topics):
        a = cpp_topics[topic]
        b = py_topics[topic]
        if len(a) != len(b):
            raise AssertionError("%s message count %d != %d"
                                 % (topic, len(a), len(b)))
        for idx, (ma, mb) in enumerate(zip(a, b)):
            if ma["log_time"] != mb["log_time"]:
                raise AssertionError("%s[%d] log_time %d != %d"
                                     % (topic, idx, ma["log_time"], mb["log_time"]))
            pa = json.loads(ma["data"].decode("utf-8"))
            pb = json.loads(mb["data"].decode("utf-8"))
            if pa != pb:
                raise AssertionError("%s[%d] payload differs: %s vs %s"
                                     % (topic, idx, pa, pb))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate a C++ MCAP file")
    parser.add_argument("--mcap", required=True)
    parser.add_argument("--scenario")
    parser.add_argument("--compare-py", help="optional Python MCAP to compare")
    parser.add_argument("--expect-points", type=int, default=None)
    args = parser.parse_args(argv)

    reader = McapReader(args.mcap)
    report = edr_verify.Report()
    by_topic = reader.messages_by_topic()
    edr_verify.check_structure(report, reader)
    edr_verify.check_channels(report, reader)
    edr_verify.check_monotonic(report, reader)
    edr_verify.check_t0(report, by_topic)
    edr_verify.check_relative_time(report, reader)
    edr_verify.check_no_absolute_time(report, reader)
    edr_verify.check_metadata(report, reader)
    edr_verify.check_file_complete(report, reader, by_topic)
    if args.scenario:
        scenario = edr_verify.load_scenario(args.scenario)
        edr_verify.check_class_a(report, by_topic, scenario, reader)
    if not report.ok():
        print(report.render(), file=sys.stderr)
        return 1
    check_schemas_and_indexes(reader, args.expect_points)
    if args.compare_py:
        compare_with_python_export(args.mcap, args.compare_py)
    return 0


if __name__ == "__main__":
    sys.exit(main())
