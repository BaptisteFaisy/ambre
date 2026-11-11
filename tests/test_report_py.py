"""Autonomous tests for the expert reading report (spec 12.12, EXP-05).

Run with:  python3 tests/test_report_py.py
Exit code: number of failing tests (0 = green).

The journal image fixture is REUSED from tests/test_mcap_py.py (which builds
it block by block in pure Python): one sealed+locked event with 11 class A
points, 3 continuation records, 200 class B samples, one gap, and one open
event. The MCAP fixture is produced by the real exporter.
"""

import hashlib
import html.parser
import os
import re
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "host", "mcap"))
sys.path.insert(0, os.path.join(ROOT, "host", "report"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import expert_report                                          # noqa: E402
import report_html                                            # noqa: E402
from ambre_journal_reader import JournalReader, crc32c        # noqa: E402
import export_mcap                                            # noqa: E402
from test_mcap_py import T0_US, build_image_bytes             # noqa: E402


class BalanceParser(html.parser.HTMLParser):
    """Minimal tag-balance checker built on the stdlib html.parser."""

    VOID = {"meta", "br", "img", "hr", "link", "input"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.errors = []

    def handle_starttag(self, tag, attrs):
        if tag not in self.VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag in self.VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.errors.append("</%s> does not close %s" % (tag, self.stack[-1:]))
        else:
            self.stack.pop()


def chained_crc(image_path, event_id):
    """CRC-32C chained over the event records (same algorithm as exporter)."""
    view = JournalReader(image_path).read_event(event_id)
    value = 0
    for record in view.records:
        value = crc32c(record["payload"], value)
    return "%08x" % value


class TestReportImage(unittest.TestCase):
    """The report generated from a raw journal image."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.image = os.path.join(cls.tmp.name, "journal.img")
        cls.image_bytes = build_image_bytes()
        with open(cls.image, "wb") as handle:
            handle.write(cls.image_bytes)
        cls.out = os.path.join(cls.tmp.name, "report.html")
        cls.rc = expert_report.main(["--image", cls.image, "--out", cls.out])
        with open(cls.out, "r", encoding="utf-8") as handle:
            cls.html = handle.read()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_exit_code_is_zero(self):
        self.assertEqual(self.rc, 0)

    def test_html_parses_and_is_balanced(self):
        parser = BalanceParser()
        parser.feed(self.html)
        parser.close()
        self.assertEqual(parser.errors, [])
        self.assertEqual(parser.stack, [], "unclosed tags: %s" % parser.stack)

    def test_standalone_no_external_reference(self):
        self.assertNotIn("<script", self.html)
        self.assertNotIn("http://", self.html)
        self.assertNotIn("https://", self.html)
        self.assertIn("<style>", self.html)

    def test_every_required_element_present(self):
        for required in report_html.REQUIRED_IDS:
            self.assertIn('id="%s"' % required, self.html,
                          "missing element %s" % required)

    def test_no_forbidden_data(self):
        for pattern, label in expert_report.FORBIDDEN_PATTERNS:
            self.assertIsNone(re.search(pattern, self.html),
                              "forbidden data (%s) found" % label)

    def test_self_check_flags_planted_violations(self):
        planted = '<div id="x">VIN ABC and 2026-11-11</div>'
        problems = expert_report.self_check(planted)
        self.assertTrue(any("vehicle identification" in p for p in problems))
        self.assertTrue(any("calendar date" in p for p in problems))

    def test_values_match_the_image(self):
        expected = [
            "25000000",                     # T0, monotonic us
            "non-reversible restraint deployed",   # trig reason 3
            "yes (never overwritten)",      # locked
            "1200 mm/s (4.32 km/h)",        # max delta-v long from the seal
            "21000 ms",                     # time since first event
            "ignition cycle 2 at download", # seal ignition_cycle_at_read
            "2 event(s) in the journal",    # sealed + open event
            "60 ms",                        # time between events 1 and 2
            "signal 40: 40",                # class B assistance samples
            hashlib.sha256(self.image_bytes).hexdigest(),
            chained_crc(self.image, 1),
        ]
        for needle in expected:
            self.assertIn(needle, self.html, "missing value %r" % needle)

    def test_precrash_points_rendered(self):
        self.assertIn("<td>-5000</td>", self.html)   # oldest class A point
        self.assertIn("<td>-0</td>", self.html)      # the T0 point
        self.assertIn("11 points", self.html)        # 5 s at 2 Hz + T0


class TestReportMcap(unittest.TestCase):
    """The report generated from an MCAP export of the same event."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.image = os.path.join(cls.tmp.name, "journal.img")
        with open(cls.image, "wb") as handle:
            handle.write(build_image_bytes())
        cls.mcap = os.path.join(cls.tmp.name, "event.mcap")
        export_mcap.export_event(JournalReader(cls.image), 1, cls.mcap, cls.image)
        cls.facts = expert_report.facts_from_mcap(cls.mcap)
        cls.html = report_html.render_report(cls.facts)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_event_fields_match_the_export(self):
        event = self.facts["event"]
        self.assertEqual(event["event_id"], 1)
        self.assertTrue(event["locked"])
        self.assertTrue(event["file_complete"])
        self.assertEqual(event["t0_ts_us"], T0_US)
        self.assertEqual(event["trig_reason_name"],
                         "non-reversible restraint deployed")

    def test_integrity_carries_the_image_digest(self):
        with open(self.image, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        self.assertEqual(self.facts["integrity"]["image_sha256"], digest)
        self.assertEqual(self.facts["integrity"]["event_payload_crc32c"],
                         chained_crc(self.image, 1))

    def test_html_passes_self_check(self):
        self.assertEqual(expert_report.self_check(self.html), [])

    def test_class_b_messages_counted(self):
        self.assertGreater(self.facts["class_b"]["total"], 0)


def main():
    suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return len(result.failures) + len(result.errors)


if __name__ == "__main__":
    sys.exit(main())
