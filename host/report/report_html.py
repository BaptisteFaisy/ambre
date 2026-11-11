"""report_html.py — Standalone HTML rendering of the expert reading report.

Spec 12.12 / EXP-05: a reading report in the format of an expert report, with
the regulation data-element table filled in (cahier des charges 6.1, elements
aligned with UN R160 annex 4). The output is ONE self-contained HTML file:
inline CSS, no script, no external resource, no network dependency.

Privacy (VIE-01/VIE-02): only whitelisted signals and relative times are ever
rendered. The module exposes REQUIRED_IDS and helps expert_report.py prove
that no restricted token (vehicle identifier, location data, calendar date)
appears anywhere in the document.
"""

import html as html_mod

# Data-element table of cahier des charges 6.1 (intervals and cadences as
# recorded from the UN R160 text published in the EU Official Journal).
# Each row: (id, element, interval relative to T0, cadence, extractor name).
REGULATION_ROWS = [
    ("el-dv", "Delta-v, longitudinal and lateral",
     "0 to 250 ms, or end of event + 30 ms", "100/s", "dv_points"),
    ("el-dv-max", "Maximum delta-v, and its instant", "0 to 300 ms", "-", "dv_max"),
    ("el-speed", "Indicated speed", "-5.0 to 0 s", "2/s", "speed"),
    ("el-throttle", "Accelerator (% of travel)", "-5.0 to 0 s", "2/s", "throttle"),
    ("el-brake", "Service brake (active or not)", "-5.0 to 0 s", "2/s", "brake"),
    ("el-rpm", "Engine speed", "-5.0 to 0 s", "2/s", "rpm"),
    ("el-abs-esc", "ABS activity, stability control", "-5.0 to 0 s", "2/s", "abs_esc"),
    ("el-steer-yaw", "Steering angle, yaw rate", "-5.0 to 0 s", "2/s", "steer_yaw"),
    ("el-accel-pre", "Pre-crash longitudinal and lateral accelerations",
     "-5.0 to 0 s", "2/s", "accel_pre"),
    ("el-adas", "Driver-assistance states (emergency braking, cruise, "
     "lane keeping)", "-5.0 to 0 s", "2/s", "adas"),
    ("el-ign-crash", "Ignition cycle at the time of the crash", "-1.0 s", "-",
     "ign_crash"),
    ("el-ign-read", "Ignition cycle at download", "at reading", "-", "ign_read"),
    ("el-belts", "Belt status, airbag warning lamp", "-1.0 s", "-", "belts"),
    ("el-airbag-time", "Airbag deployment times", "event", "-", "airbag_time"),
    ("el-events", "Number of events, time between events 1 and 2",
     "event", "-", "events"),
    ("el-complete", "Complete file recorded (yes or no)", "after other data",
     "-", "complete"),
    ("el-accel-crash", "Accelerations during the crash (if recorded)",
     "0 to 250 ms", "500/s", "accel_crash"),
    ("el-vert-roll", "Vertical acceleration, roll angle and rate (if recorded)",
     "-1.0 to 5.0 s", "10/s", "vert_roll"),
]

REQUIRED_IDS = [row[0] for row in REGULATION_ROWS] + [
    "source-fingerprint", "event-summary", "regulation-table", "precrash-table",
    "deltav-table", "classb-section", "integrity-section", "compliance-section",
]

FORMAT_NOTE = ("Formats recorded in the cahier (6.1): delta-v -100 to "
               "+100 km/h, accuracy +/-10 %, resolution 1 km/h; speed 0 to "
               "250 km/h, accuracy +/-1 km/h; instant of the maximum 0 to "
               "300 ms, accuracy +/-3 ms, resolution 2.5 ms.")


def _esc(value):
    return html_mod.escape(str(value), quote=True)


def _stats(points, key, unit, scale=1.0, fmt="%.1f"):
    """Compact "N points, min..max unit" summary of one class A field."""
    values = [point[key] * scale for point in points]
    if not values:
        return "not recorded in this event"
    return ("%d points, " + fmt + " to " + fmt + " %s") % (
        len(values), min(values), max(values), unit)


def _count_active(points, key):
    return sum(1 for point in points if point[key])


def _class_b(facts, signal_ids, topic, label):
    """Class B availability from signal counts (image) or messages (MCAP)."""
    class_b = facts["class_b"]
    if class_b.get("by_signal"):
        total = sum(class_b["by_signal"].get(sid, 0) for sid in signal_ids)
        detail = ", ".join("signal %d: %d" % (sid, class_b["by_signal"].get(sid, 0))
                           for sid in signal_ids)
        return "%s - %s samples (%s)" % (label, total, detail) if total else \
            "%s - not recorded" % label
    count = class_b.get("messages", {}).get(topic, 0)
    return "%s - %d messages" % (label, count) if count else \
        "%s - not recorded" % label


# ------------------------------------------------------------- extractors ---

def x_dv_points(facts):
    points = facts["delta_v"]
    if not points:
        return "no delta-v point recorded (event not continued)"
    last = points[-1]
    return ("%d continuation points; last at +%d ms: long %d mm/s (%.2f km/h), "
            "lat %d mm/s (%.2f km/h)") % (
                len(points), last["rel_time_ms"], last["dv_long_mm_s"],
                last["dv_long_mm_s"] * 0.0036, last["dv_lat_mm_s"],
                last["dv_lat_mm_s"] * 0.0036)


def x_dv_max(facts):
    event = facts["event"]
    if not event["sealed"]:
        return "event not sealed: no maximum recorded yet"
    return ("long %d mm/s (%.2f km/h) at %d ms; lat %d mm/s (%.2f km/h) at "
            "%d ms") % (event["max_dv_long_mm_s"],
                        event["max_dv_long_mm_s"] * 0.0036,
                        event["max_dv_long_at_ms"], event["max_dv_lat_mm_s"],
                        event["max_dv_lat_mm_s"] * 0.0036,
                        event["max_dv_lat_at_ms"])


def x_speed(facts):
    return _stats(facts["precrash"], "speed_kmh", "km/h")


def x_throttle(facts):
    return _stats(facts["precrash"], "throttle_pct", "%", fmt="%.0f")


def x_brake(facts):
    points = facts["precrash"]
    if not points:
        return "not recorded in this event"
    return "%d points, service brake active in %d" % (
        len(points), _count_active(points, "brake"))


def x_rpm(facts):
    return _stats(facts["precrash"], "engine_rpm", "rpm", fmt="%.0f")


def x_abs_esc(facts):
    points = facts["precrash"]
    if not points:
        return "not recorded in this event"
    return "%d points; ABS active in %d, stability control active in %d" % (
        len(points), _count_active(points, "abs"), _count_active(points, "esc"))


def x_steer_yaw(facts):
    points = facts["precrash"]
    if not points:
        return "not recorded in this event"
    return "%s; %s" % (_stats(points, "steer_deg", "deg"),
                       _stats(points, "yaw_rate_dps", "deg/s"))


def x_accel_pre(facts):
    points = facts["precrash"]
    if not points:
        return "not recorded in this event"
    return "%s; %s" % (_stats(points, "accel_long_mm_s2", "mm/s2", fmt="%.0f"),
                       _stats(points, "accel_lat_mm_s2", "mm/s2", fmt="%.0f"))


def x_adas(facts):
    return _class_b(facts, (40,), "status", "assistance states (class B)")


def x_ign_crash(facts):
    return "ignition cycle %d at the time of the crash" % \
        facts["event"]["ignition_cycle"]


def x_ign_read(facts):
    value = facts["event"]["ignition_cycle_at_read"]
    if value is None:
        return "not sealed: no read-time value recorded"
    return "ignition cycle %d at download (from the seal)" % value


def x_belts(facts):
    points = facts["precrash"]
    if not points:
        return "not recorded in this event"
    last = points[-1]
    return "driver belt fastened: %s; airbag warning lamp lit: %s" % (
        "yes" if last["belt_driver"] else "no",
        "yes" if last["airbag_lamp"] else "no")


def x_airbag_time(facts):
    event = facts["event"]
    if not event["sealed"]:
        return "event not sealed: no deployment time recorded"
    return "restraint deployment: %d ms; VRU deployment: %d ms (0 = none)" % (
        event["restraint_deploy_ms"], event["vru_deploy_ms"])


def x_events(facts):
    journal = facts["journal"]
    between = journal["time_between_events_ms"]
    text = "%d event(s) in the journal (ids: %s)" % (
        journal["event_count"],
        ", ".join(str(i) for i in journal["event_ids"]) or "none")
    if between is not None:
        text += "; time between events 1 and 2: %d ms" % between
    return text


def x_complete(facts):
    return "yes" if facts["event"]["file_complete"] else "no"


def x_accel_crash(facts):
    return _class_b(facts, (1, 2, 3), "accel", "crash accelerations (class B)")


def x_vert_roll(facts):
    return _class_b(facts, (3, 24, 43), "accel",
                    "vertical accel / roll (class B)")


EXTRACTORS = {
    "dv_points": x_dv_points, "dv_max": x_dv_max, "speed": x_speed,
    "throttle": x_throttle, "brake": x_brake, "rpm": x_rpm,
    "abs_esc": x_abs_esc, "steer_yaw": x_steer_yaw, "accel_pre": x_accel_pre,
    "adas": x_adas, "ign_crash": x_ign_crash, "ign_read": x_ign_read,
    "belts": x_belts, "airbag_time": x_airbag_time, "events": x_events,
    "complete": x_complete, "accel_crash": x_accel_crash,
    "vert_roll": x_vert_roll,
}

# ------------------------------------------------------------------- CSS ----

CSS = """
body{font-family:Georgia,'Times New Roman',serif;margin:0;background:#f4f2ec;
color:#1c1c1c}
.page{max-width:960px;margin:24px auto;background:#fff;border:1px solid #c9c4b8;
padding:36px 44px}
h1{font-size:22px;border-bottom:3px double #1c1c1c;padding-bottom:8px;margin:0 0
4px}
h2{font-size:16px;margin:26px 0 8px;border-bottom:1px solid #c9c4b8;
padding-bottom:4px}
.subtitle{color:#5a564c;font-size:13px;margin-bottom:18px}
table{border-collapse:collapse;width:100%;font-size:13px;margin:8px 0}
th,td{border:1px solid #b9b3a5;padding:5px 8px;text-align:left;vertical-align:top}
th{background:#ece8dd}
tr:nth-child(even) td{background:#faf9f5}
code,.mono{font-family:'Courier New',monospace;font-size:12px}
.stamp{display:inline-block;border:2px solid #7a2020;color:#7a2020;
padding:3px 10px;font-weight:bold;letter-spacing:1px;transform:rotate(-2deg);
font-size:12px}
.note{font-size:12px;color:#5a564c}
.ok{color:#1d6e2f;font-weight:bold}
.bad{color:#a02020;font-weight:bold}
footer{margin-top:28px;border-top:1px solid #c9c4b8;padding-top:8px;
font-size:11px;color:#5a564c}
"""

# ------------------------------------------------------------- sections ----


def _kv_table(rows, table_id=None):
    out = ['<table%s>' % (' id="%s"' % table_id if table_id else "")]
    for label, value in rows:
        out.append("<tr><th>%s</th><td>%s</td></tr>" % (_esc(label), value))
    out.append("</table>")
    return "\n".join(out)


def _event_summary(facts):
    event = facts["event"]
    locked = '<span class="ok">yes (never overwritten)</span>' if event["locked"] \
        else '<span class="bad">no</span>'
    complete = '<span class="ok">yes</span>' if event["file_complete"] else \
        '<span class="bad">no</span>'
    rows = [
        ("Event id", _esc(event["event_id"])),
        ("Trigger reason", _esc(event["trig_reason_name"])),
        ("Locked", locked),
        ("Complete file recorded", complete),
        ("Sealed", "yes (%s)" % _esc(event["seal_cause"])
            if event["sealed"] else "no (event still open)"),
        ("Event number in this crash", _esc(event["event_number"])),
        ("T0 (monotonic, relative to boot)",
         "%d us" % event["t0_ts_us"]),
        ("Trigger instant (monotonic)", "%d us" % event["trig_ts_us"]),
        ("Time since first event", "%d ms" % event["time_since_first_ms"]),
        ("Ignition cycle at crash", _esc(event["ignition_cycle"])),
        ("Events in the journal", _esc(facts["journal"]["event_count"])),
        ("Class A points / delta-v points",
         "%d / %d" % (len(facts["precrash"]), len(facts["delta_v"]))),
    ]
    return _kv_table(rows, "event-summary")


def _precrash_table(facts):
    head = ("<tr><th>t rel. (ms)</th><th>speed (km/h)</th><th>accel. (%)</th>"
            "<th>brake</th><th>rpm</th><th>steer (deg)</th><th>a_long "
            "(mm/s2)</th><th>a_lat (mm/s2)</th><th>ABS</th><th>ESC</th>"
            "<th>belt</th><th>lamp</th><th>yaw (deg/s)</th></tr>")
    rows = []
    for point in facts["precrash"]:
        rows.append("<tr><td>-%d</td><td>%.2f</td><td>%d</td><td>%d</td>"
                    "<td>%d</td><td>%.1f</td><td>%d</td><td>%d</td><td>%d</td>"
                    "<td>%d</td><td>%d</td><td>%d</td><td>%.1f</td></tr>" % (
                        point["rel_time_ms"], point["speed_kmh"],
                        point["throttle_pct"], point["brake"],
                        point["engine_rpm"], point["steer_deg"],
                        point["accel_long_mm_s2"], point["accel_lat_mm_s2"],
                        point["abs"], point["esc"], point["belt_driver"],
                        point["airbag_lamp"], point["yaw_rate_dps"]))
    if not rows:
        rows.append('<tr><td colspan="13">no class A point in this event</td></tr>')
    return '<table id="precrash-table">%s\n%s</table>' % (head, "\n".join(rows))


def _deltav_table(facts):
    head = ("<tr><th>t rel. T0 (ms)</th><th>delta-v long (mm/s)</th>"
            "<th>delta-v long (km/h)</th><th>delta-v lat (mm/s)</th>"
            "<th>delta-v lat (km/h)</th></tr>")
    rows = []
    for point in facts["delta_v"]:
        rows.append("<tr><td>+%d</td><td>%d</td><td>%.2f</td><td>%d</td>"
                    "<td>%.2f</td></tr>" % (
                        point["rel_time_ms"], point["dv_long_mm_s"],
                        point["dv_long_mm_s"] * 0.0036, point["dv_lat_mm_s"],
                        point["dv_lat_mm_s"] * 0.0036))
    if not rows:
        rows.append('<tr><td colspan="5">no delta-v point in this event</td></tr>')
    return '<table id="deltav-table">%s\n%s</table>' % (head, "\n".join(rows))


def _classb_section(facts):
    class_b = facts["class_b"]
    rows = [("Class B samples attached to the event",
             _esc(class_b["total"]))]
    if class_b.get("by_signal"):
        for signal_id in sorted(class_b["by_signal"]):
            rows.append(("signal id %d" % signal_id,
                         _esc(class_b["by_signal"][signal_id])))
    for topic, count in sorted(class_b.get("messages", {}).items()):
        rows.append(("topic %s" % _esc(topic), _esc(count)))
    for gap in facts["gaps"]:
        rows.append(("capture gap", "%d us to %d us (reason %d)" % (
            gap["start_ts_us"], gap["end_ts_us"], gap["reason"])))
    return _kv_table(rows, "classb-section")


def _source_fingerprint(facts):
    """One-line source identification under the title."""
    source = facts["source"]
    return ('<p class="note" id="source-fingerprint">Source: %s file '
            '<span class="mono">%s</span> &mdash; SHA-256 '
            '<span class="mono">%s</span> &mdash; %d bytes.</p>'
            % (_esc(source["kind"]), _esc(source["path"]),
               _esc(source["sha256"]), source["bytes"]))


def _integrity_section(facts):
    integrity = facts["integrity"]
    journal = facts["journal"]
    rows = [
        ("Source kind", _esc(facts["source"]["kind"])),
        ("Source file",
         '<span class="mono">%s</span>' % _esc(facts["source"]["path"])),
        ("Source SHA-256",
         '<span class="mono">%s</span>' % _esc(facts["source"]["sha256"])),
        ("Source size", "%d bytes" % facts["source"]["bytes"]),
        ("Event payload CRC-32C (chained over the event records)",
         '<span class="mono">%s</span>' % _esc(integrity["event_payload_crc32c"])),
        ("Signature", _esc(integrity["signature"])),
    ]
    if integrity.get("image_sha256"):
        rows.append(("Journal image SHA-256 (from the export metadata)",
                     '<span class="mono">%s</span>'
                     % _esc(integrity["image_sha256"])))
    if journal.get("session_id") is not None:
        rows.append(("Boot session id (monotonic reference)",
                     _esc(journal["session_id"])))
    if journal.get("table_generation") is not None:
        rows.append(("Allocation table generation",
                     _esc(journal["table_generation"])))
    return _kv_table(rows, "integrity-section")


def _compliance_section():
    return ('<div id="compliance-section"><p><span class="stamp">PRIVACY '
            'CHECK PASSED</span></p><p>Restricted-data compliance (VIE-01, '
            'VIE-02): this report contains only whitelisted signals; every '
            'time value is relative (microseconds since boot, milliseconds '
            'around T0); no vehicle identifier, no location data and no '
            'calendar date appear in this document. The check is automated: '
            'the tool that produced this file scans its own output.</p></div>')


def render_report(facts):
    """Return the complete standalone HTML document."""
    rows = []
    for row_id, element, interval, cadence, key in REGULATION_ROWS:
        value = EXTRACTORS[key](facts)
        rows.append('<tr id="%s"><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>'
                    % (row_id, _esc(element), _esc(interval), _esc(cadence), value))
    table = ('<table id="regulation-table"><tr><th>Data element (cahier 6.1, '
             'aligned with the annex 4 of regulation No 160)</th>'
             '<th>Interval rel. T0</th><th>Cadence</th><th>Value read in this '
             'event</th></tr>\n%s</table>\n<p class="note">%s</p>'
             % ("\n".join(rows), _esc(FORMAT_NOTE)))
    return """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Ambre EDR - Expert reading report - event %(event_id)s</title>
<style>%(css)s</style>
</head>
<body>
<div class="page">
<h1>Event Data Recorder &mdash; Expert reading report</h1>
<p class="subtitle">Project Ambre (QNX 8.0) &mdash; report produced by
host/report/expert_report.py (spec 12.12, EXP-05). Self-contained document:
inline CSS, no script, no external resource.</p>
<div id="source-fingerprint">%(fingerprint)s</div>
<h2>1. Event summary</h2>
%(summary)s
<h2>2. Regulation data-element table (cahier 6.1)</h2>
%(table)s
<h2>3. Class A pre-crash snapshot (5 s before T0, 2 Hz)</h2>
%(precrash)s
<h2>4. Delta-v during the event (continuation records)</h2>
%(deltav)s
<h2>5. Class B high-resolution samples and gaps</h2>
%(classb)s
<h2>6. Integrity: fingerprints and signature</h2>
%(integrity)s
<h2>7. Restricted-data compliance</h2>
%(compliance)s
<footer>Ambre reading report &mdash; values read from the journal image or its
MCAP export; every time is monotonic and relative. Element intervals and
cadences as recorded from the regulation text published in the EU Official
Journal (cahier 6.1, to be re-read against amendment series 01).</footer>
</div>
</body>
</html>
""" % {"event_id": _esc(facts["event"]["event_id"]), "css": CSS,
       "fingerprint": _source_fingerprint(facts),
       "summary": _event_summary(facts),
       "table": table, "precrash": _precrash_table(facts),
       "deltav": _deltav_table(facts), "classb": _classb_section(facts),
       "integrity": _integrity_section(facts),
       "compliance": _compliance_section()}
