"""Independent Python 3 twin of the Ambre crash trigger engine (DEC-12).

Implements the normative arithmetic contract A1..A9 of fmt/ambre_trigger.h and
requirements DEC-01..DEC-13 (EDR QNX 8.0). Standard library only.

Integer / division model
------------------------
idiv_trunc(num, den) reproduces C truncating division (toward zero); Python's
`//` floors toward -inf and is never used for the contract divisions. Delta-v
lives in a C `int64_t`; here it is a Python int kept in the signed 64-bit range
by `to_int64()` after every update, so wrap-around matches the C engine.
"""

from dataclasses import dataclass, field
from typing import List, Optional

AMBRE_TRIG_MODE_WINDOW = 0
AMBRE_TRIG_MODE_ANCHORED = 1
AMBRE_TRIG_DT_MAX_US = 10000
AMBRE_TRIG_RING_SLOTS = 192

AMBRE_TRIG_NONE = 0
AMBRE_TRIG_DV_LONG = 1
AMBRE_TRIG_DV_LAT = 2
AMBRE_TRIG_RESTRAINT = 3
AMBRE_TRIG_DV25_LOCK = 4
AMBRE_TRIG_VRU = 5
AMBRE_TRIG_EMERG_BRAKE = 6
AMBRE_TRIG_AEB = 7
AMBRE_TRIG_SERVICE = 8

_INT64_MAX = (1 << 63) - 1
_INT64_MIN = -(1 << 63)
_INT64_MOD = 1 << 64


def to_int64(val: int) -> int:
    """Wrap an integer into the signed 64-bit range [-2^63, 2^63 - 1]."""
    u = val % _INT64_MOD
    return u - _INT64_MOD if u > _INT64_MAX else u


def idiv_trunc(num: int, den: int) -> int:
    """Integer division truncating toward zero (C99/C11 semantics)."""
    if den == 0:
        raise ZeroDivisionError("idiv_trunc: division by zero")
    q = abs(num) // abs(den)
    return -q if (num < 0) != (den < 0) else q


def mkmh_to_mm_s(mkmh: int) -> int:
    """Convert milli-km/h to mm/s with truncating division (A8)."""
    return idiv_trunc(mkmh * 5, 18)


AMBRE_DV_TRIGGER_MM_S = mkmh_to_mm_s(8000)   # 2222 mm/s (8 km/h)
AMBRE_DV_LOCK_MM_S = mkmh_to_mm_s(25000)     # 6944 mm/s (25 km/h)
AMBRE_DV_T0_MM_S = mkmh_to_mm_s(800)         # 222 mm/s (0.8 km/h)


@dataclass
class TrigConfig:
    """Trigger engine configuration (mirrors ambre_trig_cfg_t)."""
    mode: int = AMBRE_TRIG_MODE_WINDOW
    dv_trigger_mm_s: int = AMBRE_DV_TRIGGER_MM_S
    dv_lock_mm_s: int = AMBRE_DV_LOCK_MM_S
    dv_t0_mm_s: int = AMBRE_DV_T0_MM_S
    win_trigger_ms: int = 150
    win_t0_long_ms: int = 20
    win_t0_lat_ms: int = 5
    has_front_restraint: int = 1
    same_crash_window_ms: int = 5000


def cfg_default() -> TrigConfig:
    """Return the default configuration."""
    return TrigConfig()


@dataclass
class TrigExt:
    """External signals and sample validity (mirrors ambre_trig_ext_t)."""
    restraint_deployed: bool = False
    vru_activated: bool = False
    emergency_brake: bool = False
    aeb_intervention: bool = False
    service_command: bool = False
    sample_invalid: bool = False


@dataclass
class TrigOut:
    """Per-sample decision (mirrors ambre_trig_out_t)."""
    fired: bool = False
    event_end: bool = False
    locked: bool = False
    reason: int = AMBRE_TRIG_NONE
    t0_ts_us: int = 0
    event_number: int = 0
    time_since_first_ms: int = 0
    dv_long_mm_s: int = 0
    dv_lat_mm_s: int = 0
    dv_since_t0_long_mm_s: int = 0
    dv_since_t0_lat_mm_s: int = 0
    max_dv_long_mm_s: int = 0
    max_dv_lat_mm_s: int = 0
    max_dv_long_at_ms: int = 0
    max_dv_lat_at_ms: int = 0
    win_long_mm_s: int = 0
    win_lat_mm_s: int = 0


class _AxisRing:
    """Fixed-capacity ring of (ts, dv) points (A4 support structure)."""
    def __init__(self, capacity: int = AMBRE_TRIG_RING_SLOTS):
        self.capacity = capacity
        self.slots = [(0, 0)] * capacity     # (ts_us, dv) pairs
        self.head = 0
        self.count = 0

    def push(self, ts_us: int, dv: int) -> None:
        self.slots[self.head] = (ts_us, dv)
        self.head = (self.head + 1) % self.capacity
        if self.count < self.capacity:
            self.count += 1

    def min_max_since(self, cutoff_ts_us: int) -> tuple[int, int]:
        """min/max dv over entries with ts >= cutoff (inclusive, A4)."""
        if not self.count:
            return 0, 0
        slots = self.slots
        cap = self.capacity
        lo = hi = None
        for i in range(self.count):
            ts_us, dv = slots[(self.head - 1 - i) % cap]
            if ts_us < cutoff_ts_us:
                break
            if lo is None or dv < lo:
                lo = dv
            if hi is None or dv > hi:
                hi = dv
        return (0, 0) if lo is None else (lo, hi)


class TrigState:
    """Full engine state (mirrors ambre_trig_state_t)."""
    def __init__(self, cfg: Optional[TrigConfig] = None):
        self.cfg = cfg if cfg is not None else cfg_default()
        self.last_ts_us = 0
        self.have_last = False
        self.dv_long = 0
        self.dv_lat = 0
        self.ring_long = _AxisRing()
        self.ring_lat = _AxisRing()
        self.in_event = False
        self.locked = False
        self.t0_set = False
        self.t0_ts_us = 0
        self.anchor_ts_us = 0
        self.anchor_dv_long = 0
        self.anchor_dv_lat = 0
        self.have_anchor = False
        self.crash_first_t0_us = 0
        self.have_crash_group = False
        self.event_number = 0
        self.events_total = 0
        self.dv_at_t0_long = 0
        self.dv_at_t0_lat = 0
        self.max_dv_long = 0
        self.max_dv_lat = 0
        self.max_dv_long_at_ms = 0
        self.max_dv_lat_at_ms = 0
        self.last_event_end_ts_us = 0


def _window_delta(ring: _AxisRing, ts_us: int, win_ms: int) -> int:
    """max(dv) - min(dv) over the inclusive window of win_ms (A4)."""
    span = win_ms * 1000
    cutoff = 0 if ts_us < span else ts_us - span
    lo, hi = ring.min_max_since(cutoff)
    return hi - lo


def _integrate(accel: int, dt_us: int) -> int:
    """Rectangle-rule increment on the current sample (A3)."""
    return idiv_trunc(accel * dt_us, 1000000)


def _latch_t0(state: TrigState, ts_us: int, w_long: int, w_lat: int) -> None:
    """Latch T0 on the first 0.8 km/h onset; drop it once activity subsides.

    T0 is the first sample where the 20 ms long or 5 ms lat window delta
    strictly exceeds dv_t0_mm_s. Outside an event, T0 is released again as
    soon as both windows fall back to <= dv_t0, so the next burst re-latches
    its own onset ("first instant" of the current crash activity).
    """
    if state.in_event:
        return
    active = w_long > state.cfg.dv_t0_mm_s or w_lat > state.cfg.dv_t0_mm_s
    if not active:
        state.t0_set = False
        return
    if not state.t0_set:
        state.t0_set = True
        state.t0_ts_us = ts_us
        state.dv_at_t0_long = state.dv_long
        state.dv_at_t0_lat = state.dv_lat


def _anchored_excursion(state: TrigState, ts_us: int) -> tuple[int, int]:
    """Excursion anchored at the last sample where delta-v was at rest (A5)."""
    ex_long = state.dv_long - state.anchor_dv_long
    ex_lat = state.dv_lat - state.anchor_dv_lat
    if not state.have_anchor or (ex_long == 0 and ex_lat == 0):
        state.have_anchor = True
        state.anchor_ts_us = ts_us
        state.anchor_dv_long = state.dv_long
        state.anchor_dv_lat = state.dv_lat
        return 0, 0
    if ts_us - state.anchor_ts_us <= state.cfg.win_trigger_ms * 1000:
        return abs(ex_long), abs(ex_lat)
    return 0, 0


def _external_reason(ext: TrigExt) -> tuple[bool, int, bool]:
    """Decode external triggers: (fired, reason, locks) (DEC-03/10/11)."""
    if ext.restraint_deployed:
        return True, AMBRE_TRIG_RESTRAINT, True
    if ext.vru_activated:
        return True, AMBRE_TRIG_VRU, True
    if ext.emergency_brake:
        return True, AMBRE_TRIG_EMERG_BRAKE, False
    if ext.aeb_intervention:
        return True, AMBRE_TRIG_AEB, False
    if ext.service_command:
        return True, AMBRE_TRIG_SERVICE, False
    return False, AMBRE_TRIG_NONE, False


def _dynamic_reason(state: TrigState, ex_long: int,
                    ex_lat: int) -> tuple[bool, int, bool]:
    """Delta-v trigger decision: (fired, reason, locks) (DEC-01/02/04, A9)."""
    lock_on = state.cfg.has_front_restraint == 0
    if lock_on and ex_long > state.cfg.dv_lock_mm_s:
        return True, AMBRE_TRIG_DV25_LOCK, True
    if ex_long > state.cfg.dv_trigger_mm_s:      # strict, A9
        return True, AMBRE_TRIG_DV_LONG, False
    if ex_lat > state.cfg.dv_trigger_mm_s:       # strict, A9
        return True, AMBRE_TRIG_DV_LAT, False
    return False, AMBRE_TRIG_NONE, False


def _register_fire(state: TrigState, ts_us: int, reason: int,
                   locks: bool, force_t0: bool) -> None:
    """Open a new event: capture T0, grouping (DEC-09), reset extrema."""
    if force_t0 or not state.t0_set:
        state.t0_set = True
        state.t0_ts_us = ts_us
        state.dv_at_t0_long = state.dv_long
        state.dv_at_t0_lat = state.dv_lat
    state.in_event = True
    if locks:
        state.locked = True
    state.max_dv_long = 0
    state.max_dv_lat = 0
    state.max_dv_long_at_ms = 0
    state.max_dv_lat_at_ms = 0
    _assign_group(state)
    state.events_total += 1


def _assign_group(state: TrigState) -> None:
    """Group events by T0 separation < same_crash_window_ms (DEC-09)."""
    t0 = state.t0_ts_us
    win = state.cfg.same_crash_window_ms * 1000
    if (state.have_crash_group and t0 >= state.crash_first_t0_us
            and t0 - state.crash_first_t0_us < win):
        state.event_number += 1
    else:
        state.crash_first_t0_us = t0
        state.have_crash_group = True
        state.event_number = 1


def _advance_event(state: TrigState, ts_us: int, w_long: int, w_t0_long: int,
                   allow_end: bool) -> tuple[bool, int]:
    """Update extrema, DEC-04 lock elevation and DEC-06 end of event."""
    rel_ms = (ts_us - state.t0_ts_us) // 1000
    d_long = abs(state.dv_long - state.dv_at_t0_long)
    d_lat = abs(state.dv_lat - state.dv_at_t0_lat)
    if d_long > state.max_dv_long:
        state.max_dv_long = d_long
        state.max_dv_long_at_ms = max(0, min(65535, rel_ms))
    if d_lat > state.max_dv_lat:
        state.max_dv_lat = d_lat
        state.max_dv_lat_at_ms = max(0, min(65535, rel_ms))

    lock_reason = AMBRE_TRIG_NONE
    if (state.cfg.has_front_restraint == 0 and not state.locked
            and w_long > state.cfg.dv_lock_mm_s):
        state.locked = True
        lock_reason = AMBRE_TRIG_DV25_LOCK

    if not allow_end:
        return False, lock_reason
    if w_t0_long <= state.cfg.dv_t0_mm_s:        # non-strict, A9
        state.in_event = False
        state.t0_set = False
        state.have_anchor = False
        return True, lock_reason
    return False, lock_reason


def _build_out(state: TrigState, ts_us: int, fired: bool, event_end: bool,
               reason: int, w_long: int, w_lat: int) -> TrigOut:
    """Assemble the per-sample output (event fields only inside an event)."""
    ctx = state.in_event or event_end
    tsf = 0
    if ctx and state.have_crash_group and ts_us >= state.crash_first_t0_us:
        # DEC-09: time elapsed since the first T0 of the current crash group.
        tsf = (ts_us - state.crash_first_t0_us) // 1000
    return TrigOut(
        fired=fired,
        event_end=event_end,
        locked=state.locked,
        reason=reason,
        t0_ts_us=state.t0_ts_us if ctx else 0,
        event_number=state.event_number if ctx else 0,
        time_since_first_ms=tsf if ctx else 0,
        dv_long_mm_s=state.dv_long,
        dv_lat_mm_s=state.dv_lat,
        dv_since_t0_long_mm_s=(state.dv_long - state.dv_at_t0_long) if ctx else 0,
        dv_since_t0_lat_mm_s=(state.dv_lat - state.dv_at_t0_lat) if ctx else 0,
        max_dv_long_mm_s=state.max_dv_long if ctx else 0,
        max_dv_lat_mm_s=state.max_dv_lat if ctx else 0,
        max_dv_long_at_ms=state.max_dv_long_at_ms if ctx else 0,
        max_dv_lat_at_ms=state.max_dv_lat_at_ms if ctx else 0,
        win_long_mm_s=w_long,
        win_lat_mm_s=w_lat,
    )


def _integrate_sample(state: TrigState, ts_us: int, accel_long: int,
                      accel_lat: int, ext: TrigExt) -> None:
    """A3 + DEC-13: advance the clock, integrate unless the sample is invalid."""
    dt_us = 0
    if state.have_last and ts_us >= state.last_ts_us:
        dt_us = min(ts_us - state.last_ts_us, AMBRE_TRIG_DT_MAX_US)
    state.last_ts_us = ts_us
    state.have_last = True

    if not ext.sample_invalid and dt_us > 0:
        state.dv_long = to_int64(state.dv_long + _integrate(accel_long, dt_us))
        state.dv_lat = to_int64(state.dv_lat + _integrate(accel_lat, dt_us))

    state.ring_long.push(ts_us, state.dv_long)
    state.ring_lat.push(ts_us, state.dv_lat)


def push(state: TrigState, ts_us: int, accel_long: int,
         accel_lat: int, ext: Optional[TrigExt] = None) -> TrigOut:
    """Push one acceleration sample and return the decision for it."""
    if ext is None:
        ext = TrigExt()

    _integrate_sample(state, ts_us, accel_long, accel_lat, ext)

    w_trig_long = _window_delta(state.ring_long, ts_us, state.cfg.win_trigger_ms)
    w_trig_lat = _window_delta(state.ring_lat, ts_us, state.cfg.win_trigger_ms)
    w_t0_long = _window_delta(state.ring_long, ts_us, state.cfg.win_t0_long_ms)
    w_t0_lat = _window_delta(state.ring_lat, ts_us, state.cfg.win_t0_lat_ms)

    _latch_t0(state, ts_us, w_t0_long, w_t0_lat)

    fired = False
    reason = AMBRE_TRIG_NONE
    if not state.in_event:
        fired, reason, locks = _external_reason(ext)
        force_t0 = fired
        if not fired:
            if state.cfg.mode == AMBRE_TRIG_MODE_ANCHORED:
                ex_long, ex_lat = _anchored_excursion(state, ts_us)
            else:
                ex_long, ex_lat = w_trig_long, w_trig_lat
            fired, reason, locks = _dynamic_reason(state, ex_long, ex_lat)
        if fired:
            _register_fire(state, ts_us, reason, locks, force_t0)

    event_end = False
    if state.in_event:
        event_end, lock_reason = _advance_event(state, ts_us, w_trig_long,
                                                w_t0_long, allow_end=not fired)
        if lock_reason != AMBRE_TRIG_NONE and not fired:
            reason = lock_reason

    return _build_out(state, ts_us, fired, event_end, reason,
                      w_trig_long, w_trig_lat)


def event_closed(state: TrigState, ts_us: int) -> None:
    """Seal notification: record the end and expire a stale crash group."""
    state.last_event_end_ts_us = ts_us
    state.in_event = False
    state.t0_set = False
    state.have_anchor = False
    if state.have_crash_group:
        win = state.cfg.same_crash_window_ms * 1000
        elapsed = ts_us - state.crash_first_t0_us
        if ts_us >= state.crash_first_t0_us and elapsed >= win:
            state.have_crash_group = False


@dataclass
class Sample:
    ts_us: int
    accel_long: int
    accel_lat: int
    ext: TrigExt


@dataclass
class Scenario:
    scen_id: int
    cfg: TrigConfig
    samples: List[Sample] = field(default_factory=list)


def parse_ext_flags(flags: int) -> TrigExt:
    """Decode the .vec ext_flags bitmask into a TrigExt."""
    return TrigExt(
        restraint_deployed=bool(flags & 0x01),
        vru_activated=bool(flags & 0x02),
        emergency_brake=bool(flags & 0x04),
        aeb_intervention=bool(flags & 0x08),
        service_command=bool(flags & 0x10),
        sample_invalid=bool(flags & 0x20),
    )


def _cfg_from_tokens(t: List[str]) -> TrigConfig:
    """Build a TrigConfig from the numeric tokens of an S line (after <id>)."""
    return TrigConfig(
        mode=int(t[0]), dv_trigger_mm_s=int(t[1]), dv_lock_mm_s=int(t[2]),
        dv_t0_mm_s=int(t[3]), win_trigger_ms=int(t[4]), win_t0_long_ms=int(t[5]),
        win_t0_lat_ms=int(t[6]), has_front_restraint=int(t[7]),
        same_crash_window_ms=int(t[8]),
    )


def parse_vec(path: str) -> List[Scenario]:
    """Parse a .vec file into a list of Scenario objects."""
    scenarios: List[Scenario] = []
    cur: Optional[Scenario] = None
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            tag = parts[0]
            if tag == "S":
                cur = Scenario(scen_id=int(parts[1]), cfg=_cfg_from_tokens(parts[2:11]))
            elif tag == "A" and cur is not None:
                flags = int(parts[4]) if len(parts) > 4 else 0
                cur.samples.append(Sample(int(parts[1]), int(parts[2]),
                                          int(parts[3]), parse_ext_flags(flags)))
            elif tag == "E" and cur is not None:
                scenarios.append(cur)
                cur = None
    if cur is not None:
        scenarios.append(cur)
    return scenarios


def run_scenario(scen: Scenario) -> List[TrigOut]:
    """Run one scenario through the engine, returning one TrigOut per sample."""
    state = TrigState(scen.cfg)
    return [push(state, s.ts_us, s.accel_long, s.accel_lat, s.ext)
            for s in scen.samples]


def format_dec_line(scen_id: int, idx: int, o: TrigOut) -> str:
    """Format one `D ...` line (exact column order, single spaces, no trailing)."""
    return " ".join(str(v) for v in (
        "D", scen_id, idx, int(o.fired), int(o.event_end), int(o.locked),
        o.reason, o.t0_ts_us, o.event_number, o.time_since_first_ms,
        o.dv_long_mm_s, o.dv_lat_mm_s, o.dv_since_t0_long_mm_s,
        o.dv_since_t0_lat_mm_s, o.max_dv_long_mm_s, o.max_dv_lat_mm_s,
        o.max_dv_long_at_ms, o.max_dv_lat_at_ms, o.win_long_mm_s, o.win_lat_mm_s,
    ))


def dec_lines_for_scenarios(scenarios: List[Scenario]) -> List[str]:
    """Produce every `D ...` line for all scenarios, in order."""
    lines: List[str] = []
    for scen in scenarios:
        for idx, out in enumerate(run_scenario(scen)):
            lines.append(format_dec_line(scen.scen_id, idx, out))
    return lines
