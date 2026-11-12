#!/usr/bin/env bash
# build-web.sh — compile the browser emulator: the REAL Ambre sources
# (fmt/ + host/explore/ + host/simdisk/ + host/web/) to one WebAssembly module
# that the website loads. No re-implementation, no fork of the logic: the page
# runs the same ambre-explore code the CI runs.
#
# Usage:
#   ./scripts/build-web.sh                 # writes site/assets/ambre-emu.js
#   EMCC=~/emsdk/upstream/emscripten/emcc ./scripts/build-web.sh
#
# Requires Emscripten (https://emscripten.org). On this machine:
#   source ~/emsdk/emsdk_env.sh
#
# Why one file: -sSINGLE_FILE embeds the module as base64 inside the .js, so
# the page works from file:// as well as from GitHub Pages, with no fetch and
# no MIME type to get right.
#
# The two -D bounds shrink the simdisk op log for a browser: 400 000 logged
# writes would reserve more memory than a web page should. Exceeding a bound
# is never silent, the write is refused (AMBRE_ERR_TOO_BIG). tests/test_web_api.c
# asserts the shipped scenarios stay inside both bounds.
set -euo pipefail

cd "$(dirname "$0")/.."

EMCC="${EMCC:-emcc}"
OUT="${OUT:-site/assets/ambre-emu.js}"
LIMITS=host/web/ambre_web_limits.h
# One source of truth for the browser bound: tests/test_web_api.c asserts the
# shipped scenarios fit it, so it cannot drift from what the page can load.
MAX_OPS="${MAX_OPS:-$(sed -n 's/#define AMBRE_WEB_SIMDISK_MAX_OPS *\([0-9]*\)u.*/\1u/p' "$LIMITS")}"
[ -n "$MAX_OPS" ] || { echo "could not read the op-log bound from $LIMITS" >&2; exit 1; }

command -v "$EMCC" >/dev/null 2>&1 || {
  echo "emcc not found. Install Emscripten (emsdk) or set EMCC=/path/to/emcc" >&2
  exit 1
}

SRC="fmt/ambre_crc32c.c fmt/ambre_fmt_block.c fmt/ambre_fmt_record.c \
fmt/ambre_fmt_recovery.c fmt/ambre_fmt_scan.c fmt/ambre_fmt_table.c \
fmt/ambre_journal_core.c fmt/ambre_journal_event.c fmt/ambre_trigger.c \
fmt/ambre_trigger_window.c \
host/simdisk/ambre_simdisk.c \
host/explore/ambre_explore.c host/explore/ambre_explore_gen.c \
host/explore/ambre_explore_worker.c host/explore/ambre_invariants.c \
host/explore/ambre_memdisk.c host/explore/ambre_scenario.c \
host/web/ambre_web_json.c host/web/ambre_web_setup.c \
host/web/ambre_web_state.c host/web/ambre_web_sweep.c \
host/web/wasm/main_wasm.c"

# ambre_explore_run.c is deliberately NOT in the list: it forks one worker per
# core, which a browser has no use for. The state space and the counters it
# reports come from the shared helpers in ambre_explore_worker.c.

echo "building $OUT with $("$EMCC" --version | head -1)"
# shellcheck disable=SC2086
"$EMCC" -O2 -std=c11 -Wall -Wextra -Wshadow -Wpedantic -Werror \
  -Ifmt -I. -Ihost/simdisk -Ihost/explore -Ihost/web \
  -DAMBRE_SIMDISK_MAX_OPS="$MAX_OPS" \
  $SRC \
  -sSINGLE_FILE=1 -sMODULARIZE=1 -sEXPORT_NAME=AmbreEmu \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=1024MB \
  -sSTACK_SIZE=4MB -sENVIRONMENT=web,node -sEXIT_RUNTIME=0 \
  -sEXPORTED_RUNTIME_METHODS=UTF8ToString,FS,ccall,cwrap \
  -o "$OUT"

sha256sum "$OUT" | awk '{print $1}' > "$OUT.sha256"
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)) and $OUT.sha256"
echo "source commit: $(git rev-parse --short HEAD)"
