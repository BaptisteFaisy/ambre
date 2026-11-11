#!/usr/bin/env python3
"""Tests for the Ambre power-cut level (host/powercut).

Run: ``python3 tests/test_powercut_py.py``
Exit code = number of failures (0 = green). Standard library only.
Everything runs against the dry-run backend or an in-process block server:
no QNX image, no real QEMU, no orphan process left behind.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POWERCUT = os.path.join(ROOT, "host", "powercut")
sys.path.insert(0, POWERCUT)

import e1_flush_probe  # noqa: E402
import e3_cycle_timing  # noqa: E402
import faulty_block_server as fbs  # noqa: E402
import powercut_campaign  # noqa: E402
import qemu_vm  # noqa: E402


class WorkdirMixin:
    """Per-test temporary directory, always removed."""

    def setUp(self):
        self.workdir = tempfile.mkdtemp(prefix="ambre-test-powercut-")

    def tearDown(self):
        shutil.rmtree(self.workdir, ignore_errors=True)

    def path(self, name):
        return os.path.join(self.workdir, name)


# -- 1. virtual machine lifecycle (dry-run) --------------------------------

class TestQemuVMDryRun(WorkdirMixin, unittest.TestCase):

    def test_start_probe_cut_restart_cycle(self):
        vm = qemu_vm.QemuVM(backend="dry-run", workdir=self.path("vm"))
        try:
            boot_s = vm.start()
            self.assertGreaterEqual(boot_s, 0.0)
            self.assertTrue(vm.probe_ready(5.0))
            self.assertIsNotNone(vm.proc)
            pid = vm.proc.pid
            vm.power_cut()
            self.assertIsNotNone(vm.proc.poll(), "the guest must be dead")
            self.assertGreaterEqual(vm.cut_seconds, 0.0)
            cycle_s = vm.cycle()
            self.assertGreaterEqual(cycle_s, 0.0)
            self.assertNotEqual(vm.proc.pid, pid, "restart spawns a new process")
            self.assertTrue(vm.dry_run_ready())
        finally:
            vm.stop()
        self.assertIsNone(vm.proc)

    def test_guest_command_and_cleanup(self):
        # No explicit workdir: the VM owns a temporary one and must remove it.
        vm = qemu_vm.QemuVM(
            backend="dry-run",
            dry_run_event_plan=[{"at_ms": 0, "event": {"id": "e1", "complete": True}}],
            dry_run_cut_ms=10)
        own_workdir = vm.workdir
        try:
            vm.start()
            result = vm.run_guest("edrctl list --json")
            self.assertTrue(result.ok)
            self.assertEqual(
                [event["id"] for event in qemu_vm.parse_events(result.stdout)], ["e1"])
        finally:
            vm.stop()
        self.assertFalse(os.path.exists(own_workdir),
                         "the VM-owned workdir is removed by stop()")

    def test_explicit_workdir_is_kept(self):
        vm = qemu_vm.QemuVM(backend="dry-run", workdir=self.path("vm2"))
        vm.start()
        vm.stop()
        self.assertTrue(os.path.exists(self.path("vm2")),
                        "a caller-provided workdir is left in place")

    def test_parse_events_shapes(self):
        self.assertEqual(qemu_vm.parse_events(""), [])
        self.assertEqual(qemu_vm.parse_events("not json"), [])
        self.assertEqual(len(qemu_vm.parse_events('[{"id": "a"}]')), 1)
        self.assertEqual(len(qemu_vm.parse_events('{"events": [{"id": "b"}]}')), 1)
        self.assertEqual(qemu_vm.parse_events('{"id": "c"}'), [{"id": "c"}])

    def test_wait_for_is_bounded(self):
        with self.assertRaises(TimeoutError):
            qemu_vm.wait_for(lambda: False, 0.05, 0.01, "never")


# -- 2. campaign -----------------------------------------------------------

SCENARIO = """# test scenario
duration_ms 300
inject 0 evt-a
acquire 20 evt-a
inject 60 evt-b
acquire 80 evt-b
inject 200 evt-c
"""


class TestCampaign(WorkdirMixin, unittest.TestCase):

    def write_scenario(self):
        path = self.path("scenario.txt")
        with open(path, "w") as handle:
            handle.write(SCENARIO)
        return path

    def campaign(self, *extra):
        report = self.path("report.json")
        argv = ["--scenario", self.write_scenario(), "--backend", "dry-run",
                "--sweep-from-ms", "0", "--sweep-to-ms", "80", "--step-ms", "20",
                "--report", report, "--workdir", self.path("campaign")] + list(extra)
        code = powercut_campaign.main(argv)
        with open(report) as handle:
            return code, json.load(handle)

    def test_five_cuts_and_valid_report(self):
        code, report = self.campaign("--cycles", "1", "--jobs", "2")
        self.assertEqual(code, 0)
        self.assertEqual(report["tool"], "powercut_campaign")
        self.assertEqual(report["backend"], "dry-run")
        self.assertEqual(report["summary"]["cuts"], 5)
        self.assertEqual(report["summary"]["violations"], 0)
        self.assertEqual(report["summary"]["errors"], 0)
        self.assertEqual(len(report["runs"]), 5)
        self.assertEqual([run["cut_ms"] for run in report["runs"]],
                         [0, 20, 40, 60, 80])
        for run in report["runs"]:
            self.assertEqual(run["verdict"], "ok")
            self.assertIsNotNone(run["cycle_s"])
            self.assertIn("detail", run)
        self.assertTrue(os.path.exists(self.path("report.json.violations.jsonl")))
        with open(self.path("report.json.violations.jsonl")) as handle:
            self.assertEqual(handle.read(), "", "no violation expected")

    def test_resume_does_not_replay(self):
        code, first = self.campaign("--cycles", "1")
        self.assertEqual(code, 0)
        self.assertEqual(first["summary"]["cuts"], 5)
        state = self.path("report.json.state.jsonl")
        with open(state) as handle:
            lines_before = len(handle.readlines())
        code, second = self.campaign("--cycles", "1", "--resume")
        self.assertEqual(code, 0)
        with open(state) as handle:
            lines_after = len(handle.readlines())
        self.assertEqual(lines_before, 5)
        self.assertEqual(lines_after, lines_before, "--resume must replay nothing")
        self.assertEqual(second["summary"]["cuts"], 5)

    def test_unflushed_window_produces_violations(self):
        code, report = self.campaign("--cycles", "1", "--dry-run-unflushed-ms", "25")
        self.assertEqual(code, 1, "violations must make the campaign fail")
        self.assertGreater(report["summary"]["violations"], 0)
        with open(self.path("report.json.violations.jsonl")) as handle:
            entries = [json.loads(line) for line in handle if line.strip()]
        self.assertTrue(entries)
        for entry in entries:
            self.assertEqual(entry["verdict"], "unexpected_loss")
            self.assertIsInstance(entry["cut_ms"], int)

    def test_classify_buckets(self):
        verdict, detail = powercut_campaign.classify(
            ["a"], ["b"], ["a", "b"], 60)
        self.assertEqual(verdict, powercut_campaign.VERDICT_PRESENCE)
        self.assertEqual(detail["unexpected_presence"], ["b"])
        verdict, detail = powercut_campaign.classify(["a", "b"], [], ["a"], 60)
        self.assertEqual(verdict, powercut_campaign.VERDICT_LOSS)
        self.assertEqual(detail["unexpected_loss"], ["b"])
        verdict, _detail = powercut_campaign.classify(["a"], ["b"], ["a"], 60)
        self.assertEqual(verdict, powercut_campaign.VERDICT_OK)

    def test_scenario_expectations(self):
        scenario = powercut_campaign.parse_scenario(self.write_scenario())
        self.assertEqual(scenario.expected_at(10), ([], ["evt-a"]),
                         "injected but not acquired yet: must be absent")
        self.assertEqual(scenario.expected_at(30), (["evt-a"], []))
        self.assertEqual(scenario.expected_at(70), (["evt-a"], ["evt-b"]))
        self.assertEqual(scenario.event_plan()[1]["at_ms"], 80)


# -- 3. essay E3 -----------------------------------------------------------

class TestE3(WorkdirMixin, unittest.TestCase):

    def test_cycle_timing_dry_run(self):
        argv = ["--backend", "dry-run", "--cycles", "5",
                "--workdir", self.path("e3")]
        self.assertEqual(e3_cycle_timing.main(argv), 0)
        args = e3_cycle_timing.build_parser().parse_args(argv)
        records = e3_cycle_timing.run_cycles(args)
        summary = e3_cycle_timing.summarise(records)
        self.assertEqual(summary["cycles"], 5)
        self.assertTrue(summary["under_target"])
        self.assertTrue(summary["answer"].startswith("yes"))
        self.assertEqual(summary["service_failures"], 0)
        for key in ("min", "median", "p99", "max"):
            self.assertIn(key, summary["total_s"])
        self.assertGreaterEqual(summary["total_s"]["max"],
                                summary["total_s"]["min"])


# -- 4. faulty block server ------------------------------------------------

class TestFaultyBlockServer(WorkdirMixin, unittest.TestCase):

    def start_server(self, drop_ratio=1.0, log_name="flushes.jsonl"):
        backing = self.path("backing.img")
        self.log_path = self.path(log_name)
        self.server = fbs.start_server(
            backing, listen="unix:" + self.path("nbd.sock"), size=1 << 20,
            drop_ratio=drop_ratio, log_path=self.log_path)
        self.backing = backing
        self.client = e1_flush_probe.NbdClient("unix:" + self.path("nbd.sock"))
        self.client.connect()
        self.addCleanup(self.client.disconnect)
        self.addCleanup(self.server.stop)
        return self.server

    def journal(self):
        return e1_flush_probe.read_log(self.log_path)

    def backing_bytes(self, offset, length):
        with open(self.backing, "rb") as handle:
            handle.seek(offset)
            return handle.read(length)

    def test_write_retained_then_dropped_at_cut(self):
        self.start_server(drop_ratio=1.0)
        self.client.write(0, b"\xa5" * 512)
        self.assertEqual(self.client.read(0, 512), b"\xa5" * 512,
                         "unflushed write is readable in the cache")
        self.assertEqual(self.backing_bytes(0, 512), b"\x00" * 512,
                         "unflushed write is not on the medium yet")
        stats = self.server.power_cut()
        self.assertEqual(stats["dropped_sectors"], 1)
        self.assertEqual(stats["kept_sectors"], 0)
        self.assertEqual(self.backing_bytes(0, 512), b"\x00" * 512,
                         "the cut lost the unflushed write")
        self.assertEqual(self.client.read(0, 512), b"\x00" * 512)

    def test_flushed_write_survives_the_cut(self):
        self.start_server(drop_ratio=1.0)
        self.client.write(0, b"\x5a" * 512)
        self.client.flush()
        self.assertEqual(self.backing_bytes(0, 512), b"\x5a" * 512,
                         "flush makes the write durable")
        self.server.power_cut()
        self.assertEqual(self.backing_bytes(0, 512), b"\x5a" * 512,
                         "a flushed write survives the cut")

    def test_flush_orders_are_journalled_exactly(self):
        self.start_server(drop_ratio=1.0)
        self.client.write(0, b"\x11" * 512)
        self.client.write(1024, b"\x22" * 512)
        self.client.flush()
        self.client.flush()
        entries = self.journal()
        flushes = [entry for entry in entries if entry["event"] == "flush"]
        self.assertEqual(len(flushes), 2, "one journal entry per FLUSH order")
        self.assertEqual(flushes[0]["flush_count"], 1)
        self.assertEqual(flushes[0]["offset"], 0)
        self.assertEqual(flushes[0]["bytes"], 1024)
        self.assertEqual([run["offset"] for run in flushes[0]["runs"]], [0, 1024])
        self.assertEqual(flushes[1]["flush_count"], 2)
        self.assertEqual(flushes[1]["bytes"], 0, "second flush had nothing pending")
        self.assertEqual([entry["seq"] for entry in entries], [1, 2])

    def test_drop_ratio_is_respected_over_200_writes(self):
        server = self.start_server(drop_ratio=0.5)
        for index in range(200):
            server.cache.write(index * 512, bytes([index % 251 + 1]) * 512)
        self.assertEqual(len(server.cache.overlay), 200)
        stats = server.power_cut()
        dropped = stats["dropped_sectors"]
        self.assertEqual(stats["kept_sectors"] + dropped, 200)
        self.assertGreater(dropped, 70, "about half of 200 writes must be dropped")
        self.assertLess(dropped, 130, "about half of 200 writes must be dropped")
        on_medium = sum(1 for index in range(200)
                        if self.backing_bytes(index * 512, 512) ==
                        bytes([index % 251 + 1]) * 512)
        self.assertEqual(on_medium, stats["kept_sectors"],
                         "kept sectors are the ones found on the medium")

    def test_ratio_zero_keeps_everything(self):
        self.start_server(drop_ratio=0.0)
        self.client.write(0, b"\x77" * 512)
        stats = self.server.power_cut()
        self.assertEqual(stats["dropped_sectors"], 0)
        self.assertEqual(self.backing_bytes(0, 512), b"\x77" * 512)


# -- 5. essay E1 (host mode) ----------------------------------------------

class TestE1(WorkdirMixin, unittest.TestCase):

    def test_host_mode_flush_reaches_medium(self):
        result = e1_flush_probe.probe_with_nbd(self.workdir)
        self.assertTrue(result["flush_logged"])
        self.assertEqual(result["flush_orders_received"], 1)
        self.assertTrue(result["write_durable_after_flush"])
        self.assertTrue(result["readback_matches"])
        self.assertEqual(e1_flush_probe.verdict_of(result), "flush-reaches-medium")
        self.assertGreaterEqual(result["flush_latency_s"], 0.0)

    def test_guest_mode_is_pending_without_image(self):
        argv = ["--mode", "guest"]
        args = argparse.Namespace(image=None, boot_timeout=1.0)
        result = e1_flush_probe.probe_guest(args, self.workdir)
        self.assertEqual(result["verdict"], "pending")
        self.assertEqual(e1_flush_probe.verdict_of(result), "pending")
        self.assertEqual(e1_flush_probe.main(argv), 2)
        self.assertTrue(any("sync" in step["command"]
                            for step in e1_flush_probe.guest_command_plan()))


if __name__ == "__main__":
    # Exit code = number of failures (project convention for test binaries).
    program = unittest.main(verbosity=2, exit=False)
    result = program.result
    failures = len(result.failures) + len(result.errors) + len(result.unexpectedSuccesses)
    sys.exit(failures)
