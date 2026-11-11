#!/usr/bin/env python3
"""Report building and human rendering for the latency campaign.

Kept apart from measure.py (the harness) so both stay readable and under the
house rule of 500 lines per file and 50 lines per function.  Every number in
the report is a measurement of this host with backend=posix; the media share
is derived (total minus software) and labelled as such.
"""

from __future__ import annotations

import datetime
import os
import platform
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import campaign                                                   # noqa: E402
import latency_stats as stats                                     # noqa: E402

BACKEND = "posix"
LIMIT_PERF01_US = 2000
LIMIT_PERF02_US = 20000
LIMIT_PERF03_US = 500
LIMIT_PERF05_NS = 50000

NOT_MEASURABLE = [
    "PERF-02 on the reference hardware: the end-to-end freeze includes the "
    "flush of the target's eMMC; here the support is a file on NVMe with "
    "fdatasync(), so only the software share is representative",
    "PERF-06: capture within one second of kernel start-up, recovery "
    "included (needs the QNX target and its boot image)",
    "PERF-07: the three worst cases explained by a kernel trace "
    "(TraceUser/TraceKernel on QNX); the PC trace is a timestamped log file "
    "written by ambre_port_trace(), not a kernel trace",
    "PERF-08: the same measurements repeated on a Raspberry Pi 5 with a "
    "stated storage medium",
    "PERF-04 at the level-C volume (10 000 triggers under CPU and disk "
    "load): this campaign measures the trigger count it was given",
    "the QNX real-time behaviour of the freeze path: SCHED_FIFO priorities, "
    "locked memory, message passing, and the DCMD_FLUSH_CACHE propagation "
    "to the device (feasibility essay E1)",
]


# ------------------------------------------------------------------ blocks --

def protocol_block():
    """What the campaign does, in the words of the report."""
    return {
        "unit": "one journal boot per shock chunk (the live journal holds 8 "
                "event slots and does not recycle them: "
                "ambre_table_pick_victim() exists in fmt/ and is unit tested, "
                "but the journal engine never calls it)",
        "one_crash_yields": "a burst of trigger events: the capture runs the "
                            "trigger engine in WINDOW mode, which re-fires "
                            "while the 150 ms window still holds the crash "
                            "delta-v; every event of the burst is a full "
                            "trigger->acquired cycle and is measured, the "
                            "first one being the crash trigger itself",
        "clock": "CLOCK_MONOTONIC microseconds, same host for every process",
        "trace": "AMBRE_TRACE stream of edr-journal: TRIGGER(1), "
                 "WRITE_SUBMIT(2), FLUSH_ACK(3), SEAL(4)",
        "perf01": "TRIGGER trace timestamp - emerg.trig_ts_us read back from "
                  "the journal image (the capture's own stamp of the crossing "
                  "sample)",
        "perf03": "freeze measured with the journal file on tmpfs (/dev/shm), "
                  "where fdatasync() has no device cost",
        "perf05": "edr-replay's own send round-trip histogram; the quiet run "
                  "has no trigger on the path",
        "start_up_format": "the journal formats a virgin partition at "
                           "start-up; the harness drains that 48 MiB with "
                           "fdatasync() before the first trigger so the first "
                           "freeze is not paying for the format",
        "edrctl": "edrctl status/list answer 'the portability layer has no "
                  "client-side reply' on this backend "
                  "(AMBRE_PORT_HAS_CONN_REQUEST undefined), so the counters "
                  "come from the journal's own status line (SIGUSR1, OBS-02) "
                  "and the per-event trig_ts_us from the journal image",
    }


def pass_block(result, summary, disk_path, filesystem, available):
    block = {"available": available, "disk": disk_path,
             "filesystem": filesystem}
    if available:
        block.update(summary)
        block["journal_worst_freeze_us"] = result.get("journal_worst_freeze_us")
    return block


def decomposition(media_summary, software_summary):
    """Software share (measured on tmpfs) and media share (derived)."""
    if not media_summary or not software_summary:
        return {"available": False}
    out = {"available": True}
    for key in ("median", "p99", "p99_9", "max"):
        total = media_summary["freeze_us"].get(key)
        software = software_summary["freeze_us"].get(key)
        if total is None or software is None:
            continue
        out[f"freeze_total_{key}_us"] = total
        out[f"freeze_software_{key}_us"] = software
        out[f"freeze_media_{key}_us"] = total - software
    out["note"] = ("software measured on tmpfs (fdatasync has no device to "
                   "talk to); media = total - software, a derived value, not "
                   "a measurement of the device")
    return out


def worst_cases(events, software_summary, count=3):
    """The worst freezes with their line-by-line decomposition (PERF-07)."""
    baseline = None
    if software_summary:
        baseline = software_summary["write_flush_us"].get("median")
    ordered = sorted((e for e in events if e.get("freeze_us") is not None),
                     key=lambda item: item["freeze_us"], reverse=True)
    cases = []
    for event in ordered[:count]:
        media = None
        if baseline is not None and event.get("write_flush_us") is not None:
            media = event["write_flush_us"] - baseline
        cases.append({
            "run": event.get("run"),
            "event_id": event["event_id"],
            "is_first_of_crash": event.get("is_first_of_crash"),
            "perf01_us": event.get("perf01_us"),
            "dispatch_us": event.get("dispatch_us"),
            "write_flush_us": event.get("write_flush_us"),
            "freeze_us": event.get("freeze_us"),
            "seal_after_us": event.get("seal_after_us"),
            "media_share_derived_us": media,
            "trace_line_us": {
                "trigger": event.get("t_trigger_us"),
                "write_submit": event.get("t_write_submit_us"),
                "flush_ack": event.get("t_flush_ack_us"),
                "seal": event.get("t_seal_us"),
            },
        })
    return cases


# -------------------------------------------------------------- verdicts ---

def perf01_verdict(media_events):
    crash = [e["perf01_us"] for e in media_events
             if e.get("is_first_of_crash") and e.get("perf01_us") is not None]
    refire = [e["perf01_us"] for e in media_events
              if not e.get("is_first_of_crash")
              and e.get("perf01_us") is not None]
    return {
        "limit_us": LIMIT_PERF01_US,
        "measured_crash_triggers": stats.summarize(crash),
        "overshoots": len([v for v in crash if v > LIMIT_PERF01_US]),
        "refire_events_measured": stats.summarize(refire),
        "refire_note": "re-fires of the same crash are measured apart: the "
                       "capture blocks on the journal's answer, so its "
                       "interpolated sample timeline drifts behind the wall "
                       "clock during a burst",
        "status": "measured (backend=posix)",
    }


def perf02_verdict(media_summary, media_events):
    if not media_summary:
        return {"status": "not measured"}
    return {
        "limit_us": LIMIT_PERF02_US,
        "measured": media_summary["freeze_us"],
        "overshoots": len([e["freeze_us"] for e in media_events
                           if e["freeze_us"] > LIMIT_PERF02_US]),
        "status": "measured on the PC (software + NVMe file flush), NOT on "
                  "the reference hardware",
    }


def perf03_verdict(software_summary, software_events):
    if not software_summary:
        return {"status": "not measured"}
    return {
        "limit_us": LIMIT_PERF03_US,
        "measured": software_summary["freeze_us"],
        "overshoots": len([e["freeze_us"] for e in software_events
                           if e["freeze_us"] > LIMIT_PERF03_US]),
        "status": "measured (software share, journal file on tmpfs)",
    }


def requirements(media_summary, software_summary, media_events, software_events,
                 quiet, perf05_pooled_p99):
    """Verdict per requirement, with the measured value and its overshoots."""
    out = {
        "PERF-01": perf01_verdict(media_events),
        "PERF-02": perf02_verdict(media_summary, media_events),
        "PERF-03": perf03_verdict(software_summary, software_events),
        "PERF-05": {
            "limit_ns": LIMIT_PERF05_NS,
            "quiet_run": quiet,
            "campaign_pooled_p99_ns": perf05_pooled_p99,
            "status": "measured (bucket ceilings reported by edr-replay)",
        },
    }
    for key in ("PERF-04", "PERF-06", "PERF-07", "PERF-08"):
        out[key] = {"status": "not measured on target",
                    "note": "see not_measurable_without_qnx"}
    return out


# ----------------------------------------------------------------- report --

def report_header(args, bins, host, load_desc, chunks, shocks):
    return {
        "schema": "ambre.latency.campaign/1",
        "backend": BACKEND,
        "generated_utc": datetime.datetime.now(datetime.timezone.utc)
                         .isoformat(timespec="seconds"),
        "command": " ".join(sys.argv),
        "host": host,
        "kernel": platform.release(),
        "compiler": campaign.compiler_version(),
        "bins": bins,
        "scenario": {"path": os.path.abspath(args.scenario), "shocks": shocks,
                     "chunks": len(chunks),
                     "triggers_requested": args.triggers},
        "quick": bool(args.quick),
        "load": load_desc,
    }


def build_report(args, bins, workdir, load_desc, chunks, shocks,
                 media, software, quiet, software_disk):
    """The machine-readable campaign report (every number carries its pass)."""
    media_summary = campaign.summarize_pass(media) if media else None
    software_summary = campaign.summarize_pass(software) if software else None
    host = campaign.host_info(workdir)
    report = report_header(args, bins, host, load_desc, chunks, shocks)
    report["protocol"] = protocol_block()
    report["passes"] = {
        "media": pass_block(media, media_summary,
                            os.path.join(workdir, "journal-media.img"),
                            host["filesystem"], media is not None),
        "software": pass_block(software, software_summary, software_disk,
                               "tmpfs", software is not None),
    }
    report["decomposition_us"] = decomposition(media_summary, software_summary)
    report["perf05_quiet_run"] = quiet
    report["worst_cases"] = worst_cases(media["events"], software_summary)
    report["requirements"] = requirements(
        media_summary, software_summary,
        media["events"], software["events"] if software else [], quiet,
        (media_summary or {}).get("perf05_send_ns", {}).get("p99"))
    report["not_measurable_without_qnx"] = NOT_MEASURABLE
    report["events"] = {"media": media["events"],
                        "software": software["events"] if software else []}
    return report


# ----------------------------------------------------------------- output --

def fmt_stats(summary, key, unit="us"):
    block = (summary or {}).get(key) or {}
    if not block or block.get("count", 0) == 0:
        return "not measured"
    return (f"median {block['median']} {unit} | p99 {block['p99']} | "
            f"p99.9 {block['p99_9']} | max {block['max']} | n={block['count']}")


def print_header(report):
    print("=== Ambre latency campaign (backend=posix) ===")
    print(f"host    : {report['host']['cpu']} x{report['host']['cores']}, "
          f"{report['host']['ram_total_bytes'] // (1024 ** 3)} GiB RAM, "
          f"kernel {report['kernel']}, {report['host']['filesystem']}")
    print(f"load    : {report['load'].get('what', 'none')}")
    print(f"scenario: {report['scenario']['shocks']} shock(s) -> "
          f"{report['scenario']['chunks']} chunk(s), "
          f"{report['scenario']['triggers_requested']} triggers requested")


def print_latencies(report):
    media = report["passes"]["media"]
    software = report["passes"]["software"]
    quiet = report["perf05_quiet_run"] or {}
    print()
    print("PERF-01 crossing sample -> trigger pulse (limit 2000 us)")
    print(f"  crash triggers : {fmt_stats(media, 'perf01_us')}")
    print(f"  re-fires       : {fmt_stats(media, 'perf01_refire_us')}")
    print(f"                   (measured while the capture was still draining "
          "the previous freeze: reported apart, never merged)")
    print(f"  events         : {media.get('events_measured', 0)} measured "
          f"({media.get('crash_triggers', 0)} crash trigger(s), "
          f"{media.get('refire_events', 0)} re-fire(s)), "
          f"{media.get('refused_triggers', 0)} refused (8-slot table full), "
          f"{media.get('runs', 0)} journal boot(s)")
    print("PERF-02 total freeze trigger -> acquired (software + media)")
    print(f"  media pass    : {fmt_stats(media, 'freeze_us')}")
    print(f"  software pass : {fmt_stats(software, 'freeze_us')}")
    dec = report["decomposition_us"]
    if dec.get("available"):
        print(f"  decomposition : software median "
              f"{dec['freeze_software_median_us']} us + media (derived) "
              f"{dec['freeze_media_median_us']} us = total "
              f"{dec['freeze_total_median_us']} us")
        print(f"                  software p99 "
              f"{dec['freeze_software_p99_us']} us + media (derived) "
              f"{dec['freeze_media_p99_us']} us = total "
              f"{dec['freeze_total_p99_us']} us")
    print("PERF-03 software share of the freeze (limit 500 us)")
    print(f"  {fmt_stats(software, 'freeze_us')}")
    print("PERF-05 producer send round trip (limit 50 us at p99)")
    print(f"  quiet run     : {quiet.get('messages', 0)} messages, p50 <= "
          f"{quiet.get('p50_ns')} ns, p99 <= {quiet.get('p99_ns')} ns, p99.9 "
          f"<= {quiet.get('p99_9_ns')} ns, worst {quiet.get('worst_ns')} ns")
    print(f"                  journal records during that drive: "
          f"{quiet.get('journal_records')} (a GAP is forwarded "
          "synchronously and blocks the producer's send)")
    print(f"  campaign pool : p99 <= "
          f"{(media.get('perf05_send_ns') or {}).get('p99')} ns (trigger "
          "frames included: they wait for the whole freeze)")


def print_histograms(report):
    events = report["events"]["media"]
    freezes = [e["freeze_us"] for e in events]
    print()
    if freezes:
        print(stats.render_histogram(freezes, stats.FREEZE_BUCKETS_US,
                                     title="freeze (PERF-02) histogram, "
                                           "media pass"))
    crash = [e["perf01_us"] for e in events
             if e.get("is_first_of_crash") and e.get("perf01_us") is not None]
    refire = [e["perf01_us"] for e in events
              if not e.get("is_first_of_crash")
              and e.get("perf01_us") is not None]
    if crash:
        print(stats.render_histogram(crash, stats.TRIGGER_BUCKETS_US,
                                     title="PERF-01 histogram, crash triggers"))
    if refire:
        print(stats.render_histogram(refire, stats.TRIGGER_BUCKETS_US,
                                     title="PERF-01 histogram, re-fires"))


def print_worst_cases(report):
    print()
    print("worst cases (PERF-07 on the PC trace; a kernel trace needs QNX):")
    for case in report["worst_cases"]:
        print(f"  run {case['run']} event {case['event_id']}: freeze "
              f"{case['freeze_us']} us = dispatch {case['dispatch_us']} + "
              f"write/commit/flush {case['write_flush_us']} (media derived "
              f"{case['media_share_derived_us']}), PERF-01 "
              f"{case['perf01_us']} us, seal +{case['seal_after_us']} us")
        line = case["trace_line_us"]
        print(f"    trace: TRIGGER {line['trigger']} -> WRITE_SUBMIT "
              f"{line['write_submit']} -> FLUSH_ACK {line['flush_ack']} -> "
              f"SEAL {line['seal']} (monotonic us)")


def print_not_measurable(report):
    print()
    print("not measurable without QNX:")
    for item in report["not_measurable_without_qnx"]:
        print(f"  - {item}")


def print_human(report):
    print_header(report)
    print_latencies(report)
    print_histograms(report)
    print_worst_cases(report)
    print_not_measurable(report)
