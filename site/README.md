# The Ambre website

Public address: **<https://baptistefaisy.github.io/ambre/>**

Published straight from this directory by GitHub Pages. There is **no build
step and no dependency**: the pages are hand-written HTML and one stylesheet,
so what is in `site/` is exactly what is served.

## Layout

```
site/
├── index.html            the pitch, the problem, the three ideas, the proof
├── emulator.html         the live emulator: cut the power in your browser
├── validation.html       every measured number, and everything not measured
├── tutorial.html         the six-chapter codelab, presented as a page
├── 404.html              served by Pages for any unknown path
├── report/index.html     a REAL expert report, generated (see below)
├── assets/ambre.css      the design system of every page
├── assets/emulator.css   the emulator page only (house rule: < 500 lines/sheet)
├── assets/emulator.js    drives the WebAssembly module (the only page with JS)
├── assets/emulator-draw.js  canvas drawing: op trace and block maps
├── assets/ambre-emu.js   GENERATED: the explorer compiled to WebAssembly
├── assets/ambre-emu.js.sha256  GENERATED: checksum of the module above
├── assets/data/*.json    raw campaign results, downloadable
├── assets/data/scenario-web.txt  the drive the emulator replays (generated)
├── assets/data/sample-event.mcap   a real MCAP export, openable in Foxglove
└── _template.html        structural contract: not served, copied by authors
```

`_template.html` is the contract every page follows: same `<head>`, same
header/nav, same footer, `aria-current="page"` on the active link. The
stylesheet is the only asset — no CDN, no web font, no analytics, no framework.
The site opens offline and works with JavaScript disabled.

## Rules for editing a page

1. Copy the skeleton from `_template.html`; do not invent a new header.
2. Use the classes of `assets/ambre.css`; never add a `<style>` block or an
   inline `style=` attribute. If a class is missing, add it to the stylesheet.
   The emulator page is the one exception allowed a second sheet
   (`assets/emulator.css`), because the house rule caps every file at 500
   lines and `ambre.css` is already close to it.
3. **No invented number.** Every figure must exist in `docs/CAMPAIGN.md`,
   `docs/CAMPAIGN-LATENCY.md`, `docs/results/` or in a report you just ran.
4. Keep the honesty sections. Anything that needs the QNX target is labelled
   `BLOCKED(no QNX SDP)`; the one requirement that fails (PERF-05) is shown as
   failed, with its measured cause. The project is "aligned with UN Regulation
   No. 160", never "compliant" or "certified".
5. Keep each file under 500 lines (house rule).

## Regenerating the published artefacts

`report/index.html` and the JSON/MCAP files under `assets/data/` are outputs of
the real tools, not hand-written content. To refresh them after a code change:

```sh
make                                                   # build the binaries
python3 host/scenarios/generate.py --seed 2026 --duration-ms 30000 \
    --crash-at-ms 25000 --dv-kmh 14 --out /tmp/scenario.txt
./build/ambre-explore --scenario /tmp/scenario.txt --segment-limit 4 \
    --states 1 --export-image /tmp/journal.img
python3 host/mcap/export_mcap.py --image /tmp/journal.img \
    --out site/assets/data/sample-event.mcap
python3 host/report/expert_report.py --image /tmp/journal.img \
    --out site/report/index.html
./scripts/explore-campaign.sh --scenarios 12 --jobs 4 --out /tmp/campaign
cp /tmp/campaign/campaign.json site/assets/data/exploration-campaign.json
```

The report keeps its own paper-like styling on purpose: it is the unmodified
output of `host/report/expert_report.py`, which is the point of publishing it.

## The emulator page

`emulator.html` is the only page with JavaScript, and its JavaScript is not a
model of the project: `assets/ambre-emu.js` is the explorer itself
(`fmt/`, `host/explore/`, `host/simdisk/`, plus the JSON API of `host/web/`)
compiled to WebAssembly. The page feeds it a scenario, a cut point and a cut
variant, and draws the answers.

The two generated files are never hand-edited:

```sh
source ~/emsdk/emsdk_env.sh        # or any Emscripten install
make web                           # scripts/build-web.sh -> site/assets/ambre-emu.js + .sha256
make web-conformance               # browser module vs native binary, counters compared
```

`make web-conformance` runs the same exploration twice — through the module and
through `build/ambre-explore --jobs 1` — and fails on a single differing
counter; its result is committed in `docs/results/web-emulator-conformance.json`
and its numbers are quoted on the page. The op-log bound of the browser build
lives in `host/web/ambre_web_limits.h` (one source of truth: the build script
reads it, `tests/test_web_api.c` asserts the shipped scenario fits it).

The scenario the page replays is generated, like every other scenario:

```sh
python3 host/scenarios/generate.py --preset urban-frontal --seed 2026 \
    --duration-ms 3000 --crash-at-ms 2000 --out site/assets/data/scenario-web.txt
```

## Publishing

`.github/workflows/pages.yml` uploads this directory and deploys it on every
push to `main` that touches `site/**` (and on `workflow_dispatch`). Pages is
configured with `build_type: workflow`, so there is no Jekyll step: a file
named `_template.html` would be skipped by Jekyll, and nothing is skipped here.

Check a deployment with:

```sh
gh run list --workflow pages.yml --limit 1
curl -sI https://baptistefaisy.github.io/ambre/ | head -1
```
