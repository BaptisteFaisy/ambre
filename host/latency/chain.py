#!/usr/bin/env python3
"""Process orchestration for the latency campaign (POSIX backend).

One "chain" is the recorder as it runs on the PC: edr-journal (with the
AMBRE_TRACE stream enabled) + edr-capture + one edr-replay run.  Everything is
started and stopped here so the campaign cannot leave an orphan behind.

Honest note about edrctl: on this backend `edrctl status|list --json` answers
"the portability layer has no client-side reply", because target/ctl/edr_ctl.c
compiles its request/response seam only when AMBRE_PORT_HAS_CONN_REQUEST is
defined and ambre_port.h does not declare ambre_port_conn_request().  The
journal counters are therefore read from the journal's own status line, which
it logs at start-up, on SIGUSR1 (OBS-02) and at exit; the per-event
trig_ts_us is read from the journal image with host/mcap/ambre_journal_reader.
"""

from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import time

SOCKET_WAIT_S = 20.0
SETTLE_S = 0.6              # let the start-up format flush before triggering
REPLAY_TIMEOUT_S = 120.0

STATUS_RE = re.compile(r"status: (.*)$")
HIST_HEAD_RE = re.compile(
    r"send round trip .*: (\d+) messages, mean (\d+) ns, worst (\d+) ns")
HIST_PCTL_RE = re.compile(r"p50 <= (\d+) ns, p99 <= (\d+) ns, p99\.9 <= (\d+) ns")
HIST_BUCKET_RE = re.compile(r"^\s*<=\s+(\d+) ns\s*:\s*(\d+)")
HIST_OVERFLOW_RE = re.compile(r"^\s*>\s+(\d+) ns\s*:\s*(\d+)")
UINT64_MAX = 18446744073709551615


# --------------------------------------------------------------- processes --

def wait_for_socket(path, timeout=SOCKET_WAIT_S):
    """True when the socket file appeared before the timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return os.path.exists(path)


def terminate(proc, timeout=5.0):
    """SIGTERM, wait, then SIGKILL: the recorder has no stop order (SEC-02)."""
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.terminate()
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            pass
    except OSError:
        pass


def drain_disk(disk_path):
    """Push the start-up format to the device before the first trigger.

    A virgin partition is formatted at start-up (48 MiB of zeros).  That
    format is not part of a freeze, so the campaign drains it before
    measuring: without this, the first fdatasync of the run would pay for
    the whole format and the first freeze would not be representative.
    """
    try:
        fd = os.open(disk_path, os.O_RDWR)
    except OSError:
        return False
    try:
        os.fdatasync(fd)
    except OSError:
        return False
    finally:
        os.close(fd)
    return True


def start_chain(bins, workdir, index, disk_path, log_dir):
    """Start edr-journal (tracing) and edr-capture; returns the chain dict."""
    j_sock = os.path.join(workdir, f"journal-{index}.sock")
    c_sock = os.path.join(workdir, f"capture-{index}.sock")
    trace = os.path.join(workdir, f"trace-{index}.txt")
    for path in (disk_path, trace, j_sock, c_sock):
        if os.path.exists(path):
            os.unlink(path)
    env = dict(os.environ)
    env["AMBRE_TRACE"] = trace
    env["AMBRE_LOG"] = env.get("AMBRE_LOG", "2")
    journal_log = open(os.path.join(log_dir, f"journal-{index}.log"), "wb")
    capture_log = open(os.path.join(log_dir, f"capture-{index}.log"), "wb")
    journal = subprocess.Popen(
        [os.path.join(bins, "edr-journal"), "--disk", disk_path,
         "--listen", j_sock], env=env, stdout=journal_log, stderr=journal_log)
    if not wait_for_socket(j_sock):
        terminate(journal)
        raise RuntimeError(f"edr-journal did not create {j_sock}")
    capture = subprocess.Popen(
        [os.path.join(bins, "edr-capture"), "--journal", j_sock,
         "--listen", c_sock], stdout=capture_log, stderr=capture_log)
    if not wait_for_socket(c_sock):
        terminate(capture)
        terminate(journal)
        raise RuntimeError(f"edr-capture did not create {c_sock}")
    time.sleep(SETTLE_S)
    drain_disk(disk_path)
    return {"journal": journal, "capture": capture, "journal_log": journal_log,
            "capture_log": capture_log, "journal_sock": j_sock,
            "capture_sock": c_sock, "trace": trace, "disk": disk_path,
            "index": index}


def stop_chain(chain):
    """Dump the journal counters (SIGUSR1), then stop both processes."""
    request_status_dump(chain)
    terminate(chain["capture"])
    terminate(chain["journal"])
    for stream in (chain["journal_log"], chain["capture_log"]):
        try:
            stream.close()
        except OSError:
            pass


def request_status_dump(chain, settle=0.15):
    """Ask the journal for a status line (OBS-02) and let it hit the log."""
    if chain["journal"].poll() is None:
        try:
            chain["journal"].send_signal(signal.SIGUSR1)
        except OSError:
            pass
    chain["journal_log"].flush()
    time.sleep(settle)


# ------------------------------------------------------------------ parsing --

def read_journal_status(log_path):
    """Last `status: key=value ...` line of a journal log, as a dict."""
    latest = None
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = STATUS_RE.search(line)
                if match:
                    latest = match.group(1)
    except OSError:
        return {}
    if latest is None:
        return {}
    out = {}
    for item in latest.split():
        key, _, value = item.partition("=")
        out[key] = int(value) if value.isdigit() else value
    return out


def parse_replay_output(text):
    """edr-replay's own PERF-05 histogram, parsed (bucket ceilings in ns)."""
    result = {"messages": 0, "mean_ns": None, "worst_ns": None,
              "p50_ns": None, "p99_ns": None, "p99_9_ns": None,
              "buckets": [], "overflow": None, "verdict": None}
    head = HIST_HEAD_RE.search(text)
    if head:
        result["messages"] = int(head.group(1))
        result["mean_ns"] = int(head.group(2))
        result["worst_ns"] = int(head.group(3))
    pctl = HIST_PCTL_RE.search(text)
    if pctl:
        result["p50_ns"] = int(pctl.group(1))
        result["p99_ns"] = int(pctl.group(2))
        result["p99_9_ns"] = int(pctl.group(3))
    for line in text.splitlines():
        bucket = HIST_BUCKET_RE.match(line)
        if bucket:
            result["buckets"].append([int(bucket.group(1)),
                                      int(bucket.group(2))])
            continue
        overflow = HIST_OVERFLOW_RE.match(line)
        if overflow:
            result["overflow"] = [int(overflow.group(1)),
                                  int(overflow.group(2))]
    verdict = re.search(r"verdict: p99 (\w+)", text)
    if verdict:
        result["verdict"] = verdict.group(1)
    return result


def run_replay(bins, chain, scenario_path, timeout=REPLAY_TIMEOUT_S):
    """One edr-replay run at real-time rate; returns (rc, stdout+stderr)."""
    proc = subprocess.run(
        [os.path.join(bins, "edr-replay"), "--capture", chain["capture_sock"],
         "--scenario", scenario_path, "--rate", "1.0"],
        capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout + proc.stderr


def read_event_descriptors(disk_path):
    """{event_id: descriptor} read back from the journal image."""
    try:
        from ambre_journal_reader import JournalReader
        reader = JournalReader(disk_path)
        return {ev["event_id"]: ev for ev in reader.events()}
    except Exception:                                   # noqa: BLE001
        return {}


def parse_trace(path):
    """{event_id: {trace point: timestamp_us}} of one AMBRE_TRACE file."""
    events = {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                fields = line.split()
                if len(fields) < 3:
                    continue
                stamp, event_id, point = (int(fields[0]), int(fields[1]),
                                          int(fields[2]))
                events.setdefault(event_id, {})[point] = stamp
    except OSError:
        return {}
    return events
