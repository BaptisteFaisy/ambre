#!/usr/bin/env python3
"""One level-2 cycle: real processes, a real SIGKILL, a real recovery.

A cycle runs the scenario through replay -> capture -> journal, kills the
journal at the requested instant, restarts it on the SAME disk file (the
recovery must run, REP-01) and checks the state the specification expects
for that window (cahier 4.3) plus the invariants I1, I2, I3 and I7.

The cut instants, and what the specification expects for each:

    pre        before the trigger      nothing to keep: no event expected
    acquired   after the flush         the event must be there, and
               (trigger -> acquired)    "complete file = no"
    sealed     after the seal          the event must be there, and
                                        "complete file = yes"
    recovery   during the recovery     the next start must reach the same
                                        state (I8)

English only, Python 3 standard library only.
"""

from __future__ import annotations

import json
import os
import random
import shutil
import time

import ambre_wire as wire
import e2e_check
import e2e_env
import e2e_fidelity

CUT_PRE = "pre"
CUT_ACQUIRED = "acquired"
CUT_SEALED = "sealed"
CUT_RECOVERY = "recovery"
CUT_RANDOM = "random"

VERDICT_OK = "ok"
VERDICT_LOSS = "unexpected_loss"
VERDICT_INCONSISTENT = "inconsistent_state"
VERDICT_CHECK = "check_failed"
VERDICT_ERROR = "error"

PRE_MARGIN_MS = 300          # "pre" cuts stop this long before the impact
PROBE_TIMEOUT_S = 2.0         # one service call may wait behind the writes
PROBE_TIMEOUT_LIMIT = 3       # ... three times in a row before we give up
RANDOM_WINDOW_MS = (-500, 800)


def scenario_header(path: str) -> dict:
    """Header of a scenario file (crash_at_ms, duration_ms, ...)."""
    return e2e_fidelity.official.load_scenario(path)[0]


def primary_event(events):
    """The first event of the list (the one the crash produced)."""
    if not events:
        return None
    return min(events, key=lambda item: item["event_id"])


def window_of(timeline) -> str:
    """Cut window, from what the probe observed before the kill."""
    if timeline.get("acquired_at") is None:
        return "before_acquisition"
    if timeline.get("sealed_at") is None:
        return "acquired_open"
    return "sealed"


def _cut_due(mode, cut_ms, crash_ms, now, timeline, primary) -> bool:
    """True when the requested cut instant is reached."""
    if mode == CUT_ACQUIRED:
        return (timeline["acquired_at"] is not None and primary is not None
                and primary["state"] == "open")
    if mode == CUT_SEALED:
        return timeline["sealed_at"] is not None
    if mode == CUT_PRE:
        return now * 1000.0 >= (crash_ms - PRE_MARGIN_MS)
    return now * 1000.0 >= cut_ms


def _probe(link, t_replay, mode, cut_ms, crash_ms, deadline_s):
    """Poll the journal until the cut instant; returns (timeline, cut_now)."""
    timeline = {"acquired_at": None, "sealed_at": None, "freeze_us": None,
                "events": 0, "last_status": None, "probe_error": None,
                "probes": 0, "probe_timeouts": 0}
    limit = time.monotonic() + deadline_s
    timeouts = 0
    while time.monotonic() < limit:
        try:
            status = link.status()
            events = link.list_events()
        except wire.WireError as exc:
            # A busy journal answers late (it serves one message per loop
            # iteration, behind the writes of the event). A timeout is
            # retried; anything else, or three timeouts in a row, ends the
            # probe and is reported as such -- never hidden.
            timeouts += 1
            timeline["probe_timeouts"] = timeouts
            if "timeout" not in str(exc) or timeouts >= PROBE_TIMEOUT_LIMIT:
                timeline["probe_error"] = str(exc)
                break
            continue
        timeouts = 0
        timeline["probes"] += 1
        now = time.monotonic() - t_replay
        timeline["events"] = len(events)
        timeline["last_status"] = status
        if status["acquired_events"] >= 1 and timeline["acquired_at"] is None:
            timeline["acquired_at"] = now
            timeline["freeze_us"] = status["last_freeze_us"]
        primary = primary_event(events)
        if primary and primary["state"] == "sealed" and timeline["sealed_at"] is None:
            timeline["sealed_at"] = now
        if _cut_due(mode, cut_ms, crash_ms, now, timeline, primary):
            return timeline, True
        time.sleep(0.001)
    return timeline, False


def _resolve_cut(cut_at, crash_ms, rng):
    """(probe mode, cut_ms, kill during the recovery) of the requested cut.

    A recovery cut needs something to recover: the journal is first killed
    after the seal, then killed again while the recovery of the restart is
    running, then started a third time (I8).
    """
    if isinstance(cut_at, (int, float)):
        return "numeric", float(cut_at), False
    text = str(cut_at).strip()
    try:                                  # a numeric instant, in ms
        return "numeric", float(text), False
    except ValueError:
        pass
    text = text.lower()
    if text == CUT_RECOVERY:
        return CUT_SEALED, float(crash_ms), True
    if text in (CUT_PRE, CUT_ACQUIRED, CUT_SEALED):
        return text, float(crash_ms), False
    if text == CUT_RANDOM:
        low, high = RANDOM_WINDOW_MS
        return "numeric", float(rng.randint(crash_ms + low, crash_ms + high)), False
    raise e2e_env.BenchError("unknown cut point: %r" % (cut_at,))


def _event_signature(events):
    """Comparable fingerprint of an event list (for the I8 check)."""
    return sorted((e["event_id"], e["state"], e["locked"], e["file_complete"],
                   e["trig_reason"]) for e in events)


def _restart_and_collect(bench, kill_in_recovery: bool, rng):
    """Restart the journal on the same disk and read its recovered state.

    With kill_in_recovery, the restart is killed while its recovery is
    running and started once more: both states are read and must be the
    same (I8). Returns (state, note, violations).
    """
    bench.start_journal()
    if not kill_in_recovery:
        return _collect_state(bench), None, []
    before = _collect_state(bench)
    wait_ms = rng.uniform(0.0, 20.0)
    time.sleep(wait_ms / 1000.0)
    bench.cut_journal()
    bench.start_journal()
    after = _collect_state(bench)
    note = "killed %.1f ms into the recovery, started again" % wait_ms
    violations = []
    if _event_signature(before[1]) != _event_signature(after[1]):
        violations.append("a cut during the recovery changed the final state: "
                           "%s -> %s (I8)" % (_event_signature(before[1]),
                                              _event_signature(after[1])))
    return after, note, violations


def _window_not_reached(mode, timeline):
    """Error text when the probe never saw the state the cut asks for."""
    if mode == CUT_ACQUIRED and timeline.get("acquired_at") is None:
        return ("the acquisition was never observed before the probe gave up "
                "(probe_error=%r, %s probes, %s timeouts): the journal service "
                "channel starved or the trigger did not fire"
                % (timeline.get("probe_error"), timeline.get("probes"),
                   timeline.get("probe_timeouts")))
    if mode in (CUT_SEALED, CUT_RECOVERY) and timeline.get("sealed_at") is None:
        return ("the seal was never observed before the probe gave up "
                "(probe_error=%r, %s probes, %s timeouts)"
                % (timeline.get("probe_error"), timeline.get("probes"),
                   timeline.get("probe_timeouts")))
    return None


def _collect_state(bench):
    """State of the recovered journal: status, events, per-event verify."""
    link = wire.connect_when_ready(bench.journal_sock, 10.0)
    try:
        status = link.status()
        events = link.list_events()
        verifies = {}
        for event in events:
            try:
                verifies[event["event_id"]] = link.verify_event(event["event_id"])
            except wire.WireError as exc:
                verifies[event["event_id"]] = {"error": str(exc)}
        return status, events, verifies
    finally:
        link.close()


def _probe_edrctl(bench):
    """Try the real edrctl binary and record its answer verbatim.

    The frozen CLI is honoured, but today's binary answers "list
    unavailable": target/port/ambre_port.h has no client-side receive
    (ambre_port_conn_request), a gap documented in target/ctl/edr_ctl.c.
    The bench never hides that; it reports which probe answered.
    """
    binary = bench.bins.get("edrctl")
    if not binary:
        return {"binary": None, "used": False}
    rc, out, err = wire.run_edrctl(binary, bench.journal_sock,
                                   ["list", "--json"], timeout=10.0)
    usable = False
    if rc == 0 and out.strip():
        try:
            json.loads(out.strip().splitlines()[0])
            usable = True
        except ValueError:
            usable = False
    return {"binary": binary, "rc": rc, "used": usable,
            "stdout": out.strip()[:400], "stderr": err.strip()[:200]}


def _classify(timeline, events, window):
    """Verdict and violations for the recovered state (I2, I3, I7)."""
    violations = []
    primary = primary_event(events)
    if timeline.get("acquired_at") is not None and primary is None:
        violations.append("the event was acquired before the cut and is gone "
                           "after the recovery (I2)")
        return VERDICT_LOSS, violations
    if primary is None:
        return VERDICT_OK, violations
    if primary["state"] != "sealed":
        violations.append("the recovered event is still %r (the recovery must "
                           "seal it)" % primary["state"])
    if window == "acquired_open" and primary["file_complete"]:
        violations.append("the event was not sealed at the cut but comes back "
                           "with complete file = yes (I7)")
    if window == "sealed" and not primary["file_complete"]:
        violations.append("the event was sealed at the cut but comes back "
                           "with complete file = no (I7)")
    for event in events:
        if event["trig_reason"] == "none":
            violations.append("event %d exists without any trigger (I3)"
                              % event["event_id"])
    verdict = VERDICT_INCONSISTENT if violations else VERDICT_OK
    return verdict, violations


def _merge_verdict(verdict: str, checks: dict) -> str:
    """Fold the export/verification outcome into the cycle verdict.

    A check failure is kept apart from an invariant violation: the first is
    something the exported file does not satisfy, the second is lost or
    inconsistent data. Both are reported, neither is hidden.
    """
    if verdict != VERDICT_OK:
        return verdict
    official = checks.get("official", {})
    fidelity = checks.get("fidelity", {})
    if not official.get("ok") or not fidelity.get("ok"):
        return VERDICT_CHECK
    return verdict


def _preserve(artifact_dir: str, bench, result: dict) -> None:
    """Keep the parameters (and, for a real violation, the disk image) of a
    cycle that went wrong: every violation found in development becomes a
    non-regression test (spec 7.7). A plain error keeps only its parameters,
    so a flaky harness cannot fill the disk with 48 MiB images."""
    os.makedirs(artifact_dir, exist_ok=True)
    image = None
    if result.get("violations"):
        image = os.path.join(artifact_dir, "journal.img")
        try:
            shutil.copyfile(bench.disk, image)
        except OSError as exc:
            image = "copy failed: %s" % exc
    params = {key: result.get(key) for key in
              ("cut", "mode", "cut_ms", "crash_ms", "window", "kill_at_ms",
               "violations", "error", "timeline", "events", "verifies")}
    params["disk_image"] = image
    params["journal_log"] = result.get("journal_log")
    params["checks"] = result.get("checks")
    export_dir = os.path.join(bench.workdir, "export")
    if os.path.isdir(export_dir):
        kept = os.path.join(artifact_dir, "export")
        shutil.copytree(export_dir, kept, dirs_exist_ok=True)
        params["export_dir"] = kept
    with open(os.path.join(artifact_dir, "params.json"), "w") as handle:
        json.dump(params, handle, indent=2, sort_keys=True, default=str)


def _cut_phase(bench, scenario, mode, cut_ms, crash_ms, rate, timeout_s):
    """Play the scenario and kill the journal at the requested instant.

    Returns (timeline, kill_at, t_replay). Raises BenchError when the window
    the caller asked for was never observed: a cycle that did not test what
    was asked is an error, never a pass.
    """
    bench.start_journal()
    bench.start_capture()
    t_replay = time.monotonic()
    bench.start_replay(scenario, rate=rate)
    link = wire.connect_when_ready(bench.journal_sock, 5.0,
                                   timeout=PROBE_TIMEOUT_S)
    try:
        timeline, _cut = _probe(link, t_replay, mode, cut_ms, crash_ms,
                                timeout_s)
    finally:
        link.close()
    missing = _window_not_reached(mode, timeline)
    if missing:
        raise e2e_env.BenchError(missing)
    return timeline, bench.cut_journal(), t_replay


def _recover_phase(bench, result, kill_in_recovery, rng, scenario, verify):
    """Restart on the same disk, read the recovered state, classify, verify."""
    (status, events, verifies), note, i8 = _restart_and_collect(
        bench, kill_in_recovery, rng)
    result["recovery_cut"] = note
    result["status"] = status
    result["events"] = events
    result["verifies"] = verifies
    result["edrctl"] = _probe_edrctl(bench)
    verdict, violations = _classify(result["timeline"], events,
                                    result["window"])
    result["violations"] = violations + i8
    result["verdict"] = (VERDICT_INCONSISTENT if result["violations"]
                         else verdict)
    if verify and events:
        result["checks"] = e2e_check.export_and_verify(
            bench, scenario, os.path.join(bench.workdir, "export"),
            primary_event(events)["event_id"])
        result["verdict"] = _merge_verdict(result["verdict"], result["checks"])


def run_cycle(bins_dir: str, scenario: str, cut_at, workdir: str = None,
              keep: bool = False, rate: float = 1.0, verify: bool = True,
              seed: int = None, timeout_s: float = 90.0,
              artifact_dir: str = None) -> dict:
    """Run one cut cycle and return its result as a plain dict."""
    started = time.monotonic()
    rng = random.Random(seed)
    header = scenario_header(scenario)
    crash_ms = int(header.get("crash_at_ms", 0))
    mode, cut_ms, kill_in_recovery = _resolve_cut(cut_at, crash_ms, rng)
    bench = e2e_env.Bench(bins_dir, workdir=workdir, keep=keep)
    result = {"cut": str(cut_at), "mode": mode, "cut_ms": cut_ms,
              "crash_ms": crash_ms, "verdict": VERDICT_ERROR,
              "violations": [], "window": None, "timeline": {},
              "events": [], "verifies": {}, "checks": {}, "edrctl": {}}
    try:
        timeline, kill_at, t_replay = _cut_phase(
            bench, scenario, mode, cut_ms, crash_ms, rate, timeout_s)
        result["timeline"] = timeline
        result["window"] = window_of(timeline)
        result["kill_at_ms"] = (kill_at - t_replay) * 1000.0
        result["replay_rc"] = e2e_env.stop_process(bench.replay)
        e2e_env.stop_process(bench.capture)
        _recover_phase(bench, result, kill_in_recovery, rng, scenario, verify)
        result["journal_log"] = bench.tail("journal.log", 6)
        if artifact_dir and (result["violations"]
                             or result["verdict"] == VERDICT_CHECK):
            _preserve(artifact_dir, bench, result)
    except Exception as exc:               # a cycle never kills the campaign
        result["error"] = "%s: %s" % (type(exc).__name__, exc)
        result["verdict"] = VERDICT_ERROR
        result["journal_log"] = bench.tail("journal.log", 20)
        if artifact_dir:
            _preserve(artifact_dir, bench, result)
    finally:
        result["cycle_s"] = round(time.monotonic() - started, 3)
        bench.cleanup()
    return result
