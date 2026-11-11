# host/report — expert reading report (spec 12.12, EXP-05)

Extreme-level item 12.12 of the cahier des charges: *"A reading report in the
format of an expert report, with the regulation table filled in."*

`expert_report.py` reads one event — from a raw journal image or from its MCAP
export — and writes a **single standalone HTML file**: inline CSS, no script,
no external resource, no network. It opens in any browser, offline.

## What the report contains

1. **Source fingerprint** — kind, path, SHA-256 and size of the input file.
2. **Event summary** — event id, trigger reason (UN R160 5.3.1/5.3.2 wording),
   locked, *complete file recorded* (yes/no), sealed + seal cause, event
   number in the crash, T0 and trigger instants (monotonic microseconds),
   time since first event, ignition cycle at the crash and at download,
   number of events in the journal and time between events 1 and 2, class A
   and delta-v point counts.
3. **Regulation data-element table** (cahier 6.1, aligned with annex 4 of
   regulation No 160): all 18 rows with *interval relative to T0* and
   *cadence*, each filled with the value read in this event; the recorded
   formats (ranges, accuracies, resolutions) are quoted below the table.
4. **Class A pre-crash snapshot** — the 5 s at 2 Hz, one row per point.
5. **Delta-v during the event** — one row per continuation record, mm/s and
   km/h.
6. **Class B samples and capture gaps** — counts per signal (image mode) or
   per topic (MCAP mode), plus every gap with its relative window.
7. **Integrity** — source SHA-256, chained CRC-32C over the event payloads,
   signature state, journal image digest when the export carries it.
8. **Restricted-data compliance** — a statement, backed by the tool's own
   automated scan of its output.

## Privacy (VIE-01, VIE-02)

Only whitelisted signals and **relative** times are rendered. The tool scans
its own HTML before writing it and refuses to emit a document matching a
vehicle identifier, location data or a calendar date; the patterns are shared
with the tests, so the check cannot rot silently.

## Reused decoders (never duplicated)

| input | decoder |
|---|---|
| raw journal image | `host/mcap/ambre_journal_reader.JournalReader` |
| MCAP export | `host/mcap/ambre_mcap_writer.McapReader` |
| class A point normalisation | `host/mcap/export_mcap.class_a_payload` |
| event summary | `host/mcap/export_mcap.event_summary_payload` |
| CRC-32C | `host/mcap/ambre_journal_reader.crc32c` |

## Usage

```sh
python3 host/report/expert_report.py --image build/journal.img --out report.html
python3 host/report/expert_report.py --image build/journal.img --event-id 2 \
        --out report.html --json facts.json
python3 host/report/expert_report.py --mcap build/event.mcap --out report.html
```

Exit code 0 when the document passed its completeness and compliance checks,
otherwise the number of problems (printed on stderr).

## Tests

```sh
python3 tests/test_report_py.py        # exit code = number of failures
```

Twelve unittest cases (standard library only), on an image fixture reused from
`tests/test_mcap_py.py` and on the real MCAP exporter: HTML parses and its
tags balance (`html.parser`), no external reference, every required element id
present, no forbidden data, the compliance scan really flags planted
violations, and every value is compared against the input image (T0, trigger
reason, lock, maximum delta-v, ignition cycle, event count, time between
events, class B counts, SHA-256, chained CRC-32C).

## Measured

| command | result |
|---|---|
| `python3 tests/test_report_py.py` | 12 tests, **OK**, exit 0 |
| `--image build-qual2/journal.img` | exit 0, 18 regulation rows, 11 class A points |
| `--mcap build-qual2/event.mcap` | exit 0, same event, digest carried from the export |

## Limits

* the report renders what the journal holds: an event with no continuation
  record shows "no delta-v point recorded" instead of zeros;
* maximum delta-v *instants* come from the seal (image mode); the MCAP export
  does not carry them, so the MCAP mode reports 0 ms and says so by absence;
* the trigger-reason wording follows `ambre_trig_reason_t`; the regulation
  text should be re-read against amendment series 01 (cahier 14).
