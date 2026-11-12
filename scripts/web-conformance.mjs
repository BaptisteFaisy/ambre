#!/usr/bin/env node
/*
 * web-conformance.mjs — Proof that the browser module is the PC tool.
 *
 * The website's emulator (site/assets/ambre-emu.js) is compiled from the same
 * C sources as build/ambre-explore. This script runs the SAME exploration
 * twice — once through the WebAssembly module, once through the native binary
 * — and compares every counter. A single differing number means the page would
 * show something the project cannot reproduce, so the script fails.
 *
 * Usage:
 *   node scripts/web-conformance.mjs                       # shipped scenario
 *   node scripts/web-conformance.mjs --states 20000 --nested
 *   node scripts/web-conformance.mjs --out docs/results/web-emulator-conformance.json
 *
 * Requires: node, build/ambre-explore (make all), site/assets/ambre-emu.js
 * (scripts/build-web.sh). Never part of the 5-minute CI budget: it is run by
 * hand whenever the emulator or the explorer changes, and the result is
 * committed under docs/results/.
 */

import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import vm from 'node:vm';

const require = createRequire(import.meta.url);

const WASM = 'site/assets/ambre-emu.js';
const EXPLORE = 'build/ambre-explore';
const CRC_CHECK = 0xe3069283;

/* Variant bits, as ambre_web_sweep_start_json() reads them. */
const SWEEP = { torn: 0x02, cache: 0x04, bitflip: 0x08, table: 0x10, nested: 0x20 };

function parseArgs(argv) {
  const opt = {
    scenario: 'site/assets/data/scenario-web.txt',
    segments: 2,
    states: 0,
    seed: 1234,
    nested: false,
    batch: 500,
    out: null,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--scenario') opt.scenario = argv[++i];
    else if (a === '--segment-limit') opt.segments = Number(argv[++i]);
    else if (a === '--states') opt.states = Number(argv[++i]);
    else if (a === '--seed') opt.seed = Number(argv[++i]);
    else if (a === '--batch') opt.batch = Number(argv[++i]);
    else if (a === '--nested') opt.nested = true;
    else if (a === '--out') opt.out = argv[++i];
    else throw new Error(`unknown option ${a}`);
  }
  return opt;
}

/* Every call goes through ccall: it converts JavaScript strings into C strings
 * (and back for the JSON answers), which a raw `mod._web_setup(str)` does not. */
function api(mod) {
  return {
    selftest: () => mod.ccall('web_crc_selftest', 'number', [], []) >>> 0,
    setup: (p, seed, dur, crash, segs) => mod.ccall('web_setup', 'number',
      ['string', 'number', 'number', 'number', 'number'], [p, seed, dur, crash, segs]),
    info: () => JSON.parse(mod.ccall('web_info', 'string', [], [])),
    trace: () => JSON.parse(mod.ccall('web_trace', 'string', [], [])),
    sweepStart: (max, mask) => JSON.parse(mod.ccall('web_sweep_start', 'string',
      ['number', 'number'], [max, mask])),
    sweepStep: (n) => JSON.parse(mod.ccall('web_sweep_step', 'string',
      ['number'], [n])),
  };
}

/* The Emscripten glue is a classic script (so the page works from file:// too).
 * Node 26 misdetects a .js file as an ES module, so load it the way a browser
 * does: evaluate the source in a CommonJS-shaped context. */
function loadWasmFactory(path) {
  const src = readFileSync(path, 'utf8');
  const abs = resolve(path);
  const sandbox = {
    module: { exports: {} }, exports: {}, require, Buffer, console, process,
    setTimeout, clearTimeout, URL, TextDecoder, TextEncoder, performance,
    __filename: abs, __dirname: dirname(abs),
  };
  sandbox.globalThis = sandbox;
  vm.runInNewContext(src, sandbox, { filename: abs });
  if (typeof sandbox.module.exports !== 'function') {
    throw new Error(`${path} did not export a module factory`);
  }
  return sandbox.module.exports;
}

/* Run the exploration inside the WebAssembly module, batch by batch, exactly
 * as the page does. */
async function runWasm(opt) {
  const mod = await loadWasmFactory(WASM)();
  const a = api(mod);
  const crc = a.selftest();
  if (crc !== CRC_CHECK) {
    throw new Error(`CRC-32C self-test failed: 0x${crc.toString(16)}`);
  }
  mod.FS.writeFile('/scenario.txt', readFileSync(opt.scenario));
  const rc = a.setup('/scenario.txt', opt.seed, 0, 0, opt.segments);
  if (rc !== 0) throw new Error(`web_setup returned ${rc}`);

  const info = a.info();
  const trace = a.trace();
  let mask = SWEEP.torn | SWEEP.cache | SWEEP.bitflip | SWEEP.table;
  if (opt.nested) mask |= SWEEP.nested;
  const start = a.sweepStart(opt.states, mask);

  let step = { done: 0 };
  const t0 = Date.now();
  while (!step.finished) {
    step = a.sweepStep(opt.batch);
  }
  step.wasm_ms = Date.now() - t0;
  return { info, trace, start, step };
}

/* The same exploration through the PC binary, single threaded (--jobs 1) so
 * the two runs are comparable state for state, not just in total. */
function runNative(opt) {
  const args = ['--scenario', opt.scenario, '--segment-limit', String(opt.segments),
                '--states', String(opt.states), '--jobs', '1',
                '--variants', 'all', '--seed', String(opt.seed)];
  if (opt.nested) args.push('--nested');
  const report = '/tmp/ambre-native-report.json';
  args.push('--report', report);
  const t0 = Date.now();
  const r = spawnSync(`./${EXPLORE}`, args, { encoding: 'utf8' });
  if (r.status !== 0 && r.status !== 1) {
    throw new Error(`ambre-explore failed (${r.status}): ${r.stderr}`);
  }
  const stats = JSON.parse(readFileSync(report, 'utf8'));
  stats.native_ms = Date.now() - t0;
  stats.stdout = (r.stdout || '').trim();
  return stats;
}

/* Every counter the two runs must agree on. */
function compare(wasm, native) {
  const diffs = [];
  const fields = ['states_explored', 'violations', 'recoveries_run',
                  'events_acquired', 'events_phantom'];
  for (const f of fields) {
    if (wasm.step[f] !== native[f]) diffs.push(`${f}: wasm ${wasm.step[f]} vs native ${native[f]}`);
  }
  for (const [k, v] of Object.entries(native.states_by_variant || {})) {
    const w = wasm.step.states_by_variant?.[k];
    if (w !== v) diffs.push(`states_by_variant.${k}: wasm ${w} vs native ${v}`);
  }
  for (const [k, v] of Object.entries(native.violations_by_invariant || {})) {
    const w = wasm.step.violations_by_invariant?.[k];
    if (w !== v) diffs.push(`violations_by_invariant.${k}: wasm ${w} vs native ${v}`);
  }
  if (wasm.start.total !== wasm.step.total) {
    diffs.push(`state space changed mid-run: ${wasm.start.total} vs ${wasm.step.total}`);
  }
  return diffs;
}

async function main() {
  const opt = parseArgs(process.argv.slice(2));
  for (const f of [WASM, EXPLORE, opt.scenario]) {
    if (!existsSync(f)) throw new Error(`missing ${f} (run make all and scripts/build-web.sh)`);
  }
  const wasm = await runWasm(opt);
  const native = runNative(opt);
  const diffs = compare(wasm, native);
  const result = {
    verdict: diffs.length === 0 ? 'PASS' : 'FAIL',
    differences: diffs,
    settings: opt,
    wasm: { states_explored: wasm.step.states_explored, violations: wasm.step.violations,
            recoveries_run: wasm.step.recoveries_run, events_acquired: wasm.step.events_acquired,
            events_phantom: wasm.step.events_phantom,
            states_by_variant: wasm.step.states_by_variant,
            violations_by_invariant: wasm.step.violations_by_invariant,
            planned_states: wasm.start.total, ms: wasm.step.wasm_ms,
            trace: wasm.trace, events: wasm.info.events.length },
    native: { states_explored: native.states_explored, violations: native.violations,
              recoveries_run: native.recoveries_run, events_acquired: native.events_acquired,
              events_phantom: native.events_phantom,
              states_by_variant: native.states_by_variant,
              violations_by_invariant: native.violations_by_invariant,
              ms: native.native_ms, stdout: native.stdout },
    artefact: { path: WASM, bytes: readFileSync(WASM).length,
                sha256: readFileSync(`${WASM}.sha256`, 'utf8').trim() },
  };
  console.log(JSON.stringify(result, null, 2));
  if (opt.out) writeFileSync(opt.out, `${JSON.stringify(result, null, 2)}\n`);
  if (diffs.length) {
    console.error(`\nCONFORMANCE FAILED: ${diffs.length} difference(s)`);
    process.exit(1);
  }
  console.error(`\nCONFORMANCE PASS: ${result.wasm.states_explored} states, ` +
                `${result.wasm.violations} violations, identical on both sides ` +
                `(wasm ${result.wasm.ms} ms, native ${result.native.ms} ms)`);
}

main().catch((e) => { console.error(e.message); process.exit(2); });
