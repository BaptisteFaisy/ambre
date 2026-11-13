# The Ambre website

Public address: **<https://tradoooo.github.io/ambre/>**

Published straight from this directory by GitHub Pages. There is **no build
step and no dependency**: the pages are hand-written HTML and one stylesheet,
so what is in `site/` is exactly what is served.

## Layout

```
site/
├── index.html            the pitch, the problem, the three ideas, the proof
├── emulator.html         THE emulator: a real QNX 8.0 guest under QEMU, live
│                          (VGA screen, serial console, injected keys)
├── explorer.html         the explorer in your browser (WebAssembly, PC backend)
├── on-target.html         the target demo: QNX 8.0 under QEMU, a real SIGKILL
│                          power cut, and the event surviving the reboot
├── validation.html       every measured number, and everything not measured
├── tutorial.html         the six-chapter codelab, presented as a page
├── 404.html              served by Pages for any unknown path
├── report/index.html     a REAL expert report, generated (see below)
├── assets/ambre.css      the design system of every page
├── assets/emulator.css   the explorer page only (house rule: < 500 lines/sheet)
├── assets/emulator.js    drives the WebAssembly module (explorer page)
├── assets/live.css       the live emulator page only
├── assets/live.js        polls the guest's console, injects keys (emulator page)
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
   Two pages carry a second sheet, because the house rule caps every file at
   500 lines and `ambre.css` is already close to it: `assets/emulator.css`
   (explorer) and `assets/live.css` (live emulator).
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

Two pages carry JavaScript, and neither is a model of the project.

`explorer.html` runs `assets/ambre-emu.js`, which is the explorer itself
(`fmt/`, `host/explore/`, `host/simdisk/`, plus the JSON API of `host/web/`)
compiled to WebAssembly (PC backend, in your browser — not the target). The page
feeds it a scenario, a cut point and a cut variant, and draws the answers.

`emulator.html` is the live one: `assets/live.js` polls the serial console of the
QNX 8.0 guest running on the r630, injects keys into it, and shows the guest's VGA
screen as a plain `<img>` fed by a multipart stream. Its base URL is written in
`assets/live.js` (`API`); if the vitrine moves, that constant moves with it.

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
curl -sI https://tradoooo.github.io/ambre/ | head -1
```
