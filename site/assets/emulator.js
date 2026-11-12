/*
 * emulator.js — drives the WebAssembly module behind site/emulator.html.
 *
 * The page owns no logic about the journal: every number comes from the C
 * module (host/web/), which is the project's own explorer compiled unchanged.
 * This file only loads it, feeds it the visitor's choices, and draws answers.
 */
(function () {
  'use strict';

  var SCENARIO_URL = 'assets/data/scenario-web.txt';
  var SCENARIO_PATH = '/scenario-web.txt';
  var SEGMENTS = 2;
  var SEED = 1234;
  var OPS_SLICE = 1200;
  var SWEEP_BATCH = 120;
  var CRC_CHECK = 0xE3069283;
  var INVARIANTS = ['I1', 'I2', 'I3', 'I4', 'I5', 'I6', 'I7', 'I8'];
  /* Invariants the C checker skips for a media fault (host/explore/
   * ambre_invariants.c): the bit that flipped is inside the data, there is no
   * redundancy to repair it, so survival cannot be demanded of that state. */
  var BITFLIP_NA = { I2: 1, I4: 1, I5: 1, I6: 1 };
  var SWEEP_MASKS = { quick: 0x1E, full: 0x3E };

  var S = { mod: null, info: null, trace: null, ops: [], cut: 0, last: null };

  function $(id) { return document.getElementById(id); }

  function setText(id, value) {
    var el = $(id);
    if (el) el.textContent = value;
  }

  function setStatus(state, message, detail) {
    var box = $('emu-status');
    if (box) box.setAttribute('data-state', state);
    setText('emu-status-msg', message);
    setText('emu-status-detail', detail || '');
  }

  function call(name, ret, types, args) {
    return S.mod.ccall(name, ret, types || [], args || []);
  }

  function ask(name, types, args) {
    return JSON.parse(call(name, 'string', types, args));
  }

  function fmt(n) {
    return typeof n === 'number' ? n.toLocaleString('en-US') : String(n);
  }

  /* ------------------------------------------------------------- loading -- */

  function loadScenarioText() {
    return fetch(SCENARIO_URL).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.text();
    }).catch(function () { return null; });
  }

  function loadOps(total) {
    var ops = [];
    for (var from = 0; from < total; from += OPS_SLICE) {
      var slice = ask('web_ops', ['number', 'number'], [from, OPS_SLICE]);
      slice.ops.forEach(function (o) { ops.push(o); });
    }
    return ops;
  }

  function selfTest() {
    var crc = call('web_crc_selftest', 'number', [], []) >>> 0;
    if (crc !== CRC_CHECK) {
      throw new Error('CRC-32C self-test gave 0x' + crc.toString(16) +
                      ', expected 0xE3069283');
    }
  }

  function setup(text) {
    var path = '';
    if (text) {
      S.mod.FS.writeFile(SCENARIO_PATH, text);
      path = SCENARIO_PATH;
    }
    var rc = call('web_setup', 'number',
                  ['string', 'number', 'number', 'number', 'number'],
                  [path, SEED, 0, 0, SEGMENTS]);
    if (rc !== 0) throw new Error('web_setup returned ' + rc);
    return text ? SCENARIO_URL : 'synthetic scenario built in C (seed ' + SEED + ')';
  }

  /* ---------------------------------------------------------------- boot -- */

  function defaultCut() {
    var ev = (S.info.events || [])[0];
    if (ev && ev.acquired_op >= 0) return ev.acquired_op + 1;
    return Math.floor(S.trace.op_count / 2);
  }

  function boot() {
    setStatus('busy', 'Loading the compiled explorer…', 'site/assets/ambre-emu.js');
    loadScenarioText().then(function (text) {
      return AmbreEmu().then(function (mod) {
        S.mod = mod;
        selfTest();
        var source = setup(text);
        S.info = ask('web_info', [], []);
        S.trace = ask('web_trace', [], []);
        S.ops = loadOps(S.trace.op_count);
        renderStatic(source);
        bindControls();
        setCut(defaultCut());
        runState();
        autoSweep();
      });
    }).catch(function (err) {
      setStatus('error', 'The emulator did not start.', String(err && err.message || err));
    });
  }

  /* ?sweep=quick|full runs the exploration on load: a shareable link for a
   * curious jury, and how the browser end-to-end check drives the sweep. */
  function autoSweep() {
    var mode = new URLSearchParams(location.search).get('sweep');
    if (mode !== 'quick' && mode !== 'full') return;
    var sel = $('emu-sweep-mode');
    if (sel) sel.value = mode;
    startSweep(mode === 'full' ? 0 : 2000, SWEEP_MASKS[mode]);
  }

  function renderStatic(source) {
    setStatus('ok', 'Ready — the real explorer is running in this tab.',
              fmt(S.trace.op_count) + ' ops · ' + fmt(S.trace.writes) +
              ' writes · ' + fmt(S.info.events.length) + ' events · CRC-32C self-test ok');
    setText('emu-drive', source);
    setText('emu-trace-summary',
            fmt(S.trace.writes) + ' writes, ' + fmt(S.trace.flushes) + ' flushes, ' +
            fmt(S.trace.marks) + ' durability marks; longest single write ' +
            fmt(S.trace.longest_write) + ' B; ' + fmt(S.trace.bytes_written) +
            ' B written in total.');
    setText('emu-events-truth', truthSummary());
    showArtefact();
  }

  function truthSummary() {
    var ev = S.info.events || [];
    if (!ev.length) return 'This scenario triggered no event.';
    var first = ev[0];
    return ev.length + ' events triggered by the impact pulse (the journal table ' +
           'holds at most 8). The first: EMERG written at op ' + first.emerg_op +
           ', acquired at op ' + first.acquired_op + ', sealed at op ' +
           first.sealed_op + (first.locked ? ' (locked)' : '') + '.';
  }

  function showArtefact() {
    fetch('assets/ambre-emu.js.sha256').then(function (r) {
      return r.ok ? r.text() : '';
    }).then(function (sha) {
      if (sha) setText('emu-sha256', sha.trim());
    }).catch(function () { /* offline: the checksum simply stays hidden */ });
  }

  /* ------------------------------------------------------------ controls -- */

  function setCut(value) {
    S.cut = Math.max(0, Math.min(S.trace.op_count, value | 0));
    var slider = $('emu-cut');
    if (slider) slider.value = String(S.cut);
    setText('emu-cut-value', fmt(S.cut) + ' / ' + fmt(S.trace.op_count));
    window.AmbreDraw.drawTimeline($('emu-timeline'), S.ops, S.cut);
    describeCut();
  }

  function describeCut() {
    var ev = S.info.events || [];
    var where = 'before any event was triggered';
    ev.forEach(function (e) {
      if (S.cut > e.sealed_op && e.sealed_op >= 0) where = 'after event ' + e.event_id + ' was sealed';
      else if (S.cut > e.acquired_op && e.acquired_op >= 0) where = 'after event ' + e.event_id + ' was acquired (durable)';
      else if (S.cut > e.emerg_op && e.emerg_op >= 0) where = 'after the EMERG record of event ' + e.event_id + ' was written, before the flush';
    });
    setText('emu-cut-where', where);
  }

  function currentVariant() {
    var checked = document.querySelector('input[name="emu-variant"]:checked');
    return checked ? Number(checked.value) : 0;
  }

  function variantParams() {
    var v = currentVariant();
    var p = { variant: v, torn: 0, lo: 0, hi: 0, off: 0, bit: 0, both: 0 };
    if (v === 1) p.torn = Number($('emu-torn').value) * 512;
    if (v === 2) {
      var mask = $('emu-mask').value;
      p.lo = mask === 'all' ? 0xFFFFFFFF : Number(mask);
      p.hi = mask === 'all' ? 0xFFFFFFFF : 0;
    }
    if (v === 3) {
      p.off = Number($('emu-offset').value);
      p.bit = Number($('emu-bit').value);
    }
    if (v === 4) p.both = $('emu-table-both').checked ? 1 : 0;
    return p;
  }

  function showParams() {
    var v = currentVariant();
    ['torn', 'cache', 'bitflip', 'table'].forEach(function (name, i) {
      var el = $('emu-params-' + name);
      if (el) el.hidden = (v !== i + 1);
    });
    var note = $('emu-variant-note');
    if (note && S.last) note.textContent = S.last.note || '';
  }

  function bindControls() {
    var slider = $('emu-cut');
    slider.max = String(S.trace.op_count);
    slider.addEventListener('input', function () { setCut(Number(slider.value)); });
    $('emu-run').addEventListener('click', runState);
    document.querySelectorAll('input[name="emu-variant"]').forEach(function (r) {
      r.addEventListener('change', function () { showParams(); runState(); });
    });
    ['emu-torn', 'emu-offset', 'emu-bit', 'emu-mask', 'emu-table-both'].forEach(function (id) {
      var el = $(id);
      if (el) el.addEventListener('change', runState);
      if (el && el.type === 'range') el.addEventListener('input', runState);
    });
    $('emu-offset').max = String(S.info.disk_size - 1);
    bindPresets();
    bindSweep();
  }

  function bindPresets() {
    var ev = (S.info.events || [])[0] || {};
    var targets = {
      start: 1,
      emerg: (ev.emerg_op || 0) + 1,
      acquired: (ev.acquired_op || 0) + 1,
      sealed: (ev.sealed_op || 0) + 1,
      end: S.trace.op_count
    };
    document.querySelectorAll('[data-cut]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        setCut(targets[btn.getAttribute('data-cut')] || 0);
        runState();
      });
    });
  }

  /* -------------------------------------------------------------- answer -- */

  function runState() {
    if (!S.mod) return;
    var p = variantParams();
    var res = ask('web_state',
                  ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
                  [S.cut, p.variant, p.torn, p.lo, p.hi, p.off, p.bit, p.both]);
    S.last = res;
    renderVerdict(res);
    renderRecovery(res);
    renderEvents(res);
    renderDisks(res);
    showParams();
  }

  function renderVerdict(res) {
    var list = $('emu-verdict');
    list.textContent = '';
    var bad = {};
    (res.violations || []).forEach(function (v) { bad[v] = 1; });
    INVARIANTS.forEach(function (name, i) {
      var li = document.createElement('li');
      var na = res.variant === 'bitflip' && BITFLIP_NA[name];
      li.className = 'emu-inv';
      li.setAttribute('data-result', na ? 'na' : (bad[name] ? 'fail' : 'pass'));
      li.textContent = name + (na ? ' n/a' : (bad[name] ? ' ✗' : ' ✓'));
      li.title = na ? 'not applicable to a media fault' : name;
      if (i === 0) li.title = 'recovery terminates within its bounds';
      list.appendChild(li);
    });
    var detail = $('emu-detail');
    detail.textContent = res.ok
      ? 'No invariant violated. Recovery terminated, the acquired event came back ' +
        'byte-identical, nothing was invented, and a second recovery gave the same table.'
      : (res.detail || 'violation');
    detail.setAttribute('data-bad', res.ok ? '0' : '1');
    setText('emu-verdict-headline', res.ok
      ? 'Survives this cut'
      : 'VIOLATION — ' + (res.violations || []).join(', '));
    setText('emu-acquired', fmt(res.acquired_before_cut));
  }

  function renderRecovery(res) {
    var body = $('emu-recovery');
    var rec = res.recovery || {};
    var rows = [['blocks scanned', rec.scanned_blocks],
                ['events before / after', rec.events_before + ' / ' + rec.events_after],
                ['torn records dropped', rec.torn_records],
                ['events sealed incomplete', rec.sealed_incomplete],
                ['table commits', rec.table_commits],
                ['table rebuilt from a scan', rec.table_rebuilt ? 'yes' : 'no'],
                ['diagnostic', rec.diag || '—']];
    body.textContent = '';
    rows.forEach(function (r) {
      var tr = document.createElement('tr');
      var th = document.createElement('th');
      var td = document.createElement('td');
      th.scope = 'row';
      th.textContent = r[0];
      td.className = 'num';
      td.textContent = typeof r[1] === 'number' ? fmt(r[1]) : String(r[1]);
      tr.appendChild(th);
      tr.appendChild(td);
      body.appendChild(tr);
    });
  }

  /* One class A point of the recovered EMERG record, as the regulation reads it. */
  function pointRow(p) {
    var tr = document.createElement('tr');
    [p[0] + ' ms', p[1].toFixed(1) + ' km/h', p[3] + '%', p[4] ? 'yes' : 'no',
     p[6] + ' mm/s²', p[9] ? 'on' : 'off', p[10] ? 'on' : 'off'].forEach(function (v) {
      var td = document.createElement('td');
      td.className = 'num';
      td.textContent = v;
      tr.appendChild(td);
    });
    return tr;
  }

  function renderEvents(res) {
    var box = $('emu-events');
    box.textContent = '';
    var events = res.events || [];
    if (!events.length) {
      var p = document.createElement('p');
      p.className = 'emu-empty';
      p.textContent = 'Nothing on the journal yet at this cut point: the recorder ' +
                      'holds no event, and the recovery correctly finds none.';
      box.appendChild(p);
      return;
    }
    events.forEach(function (ev) { box.appendChild(eventCard(ev)); });
  }

  function eventCard(ev) {
    var panel = document.createElement('div');
    panel.className = 'emu-panel';
    var h = document.createElement('h3');
    h.textContent = 'Event ' + ev.event_id + ' — ' +
      (ev.state === 2 ? 'sealed' : 'open') +
      (ev.locked ? ', locked' : '') +
      ', file complete: ' + (ev.file_complete ? 'yes' : 'no');
    panel.appendChild(h);
    panel.appendChild(eventMeta(ev));
    if (ev.emerg) panel.appendChild(emergTable(ev.emerg));
    if (ev.seal) panel.appendChild(sealList(ev.seal));
    return panel;
  }

  function eventMeta(ev) {
    var p = document.createElement('p');
    p.className = 'emu-hint';
    p.textContent = 'read back with ambre_journal_read_event(): ' +
      (ev.have_emerg ? 'EMERG present' : 'EMERG missing') + ', ' +
      ev.cont_count + ' continuation records, ' + ev.data_blocks +
      ' class B blocks, trigger reason ' + ev.trig_reason + ', blocks ' +
      ev.start_block + '–' + ev.end_block + ' of segment ' + ev.start_seg + '.';
    return p;
  }

  function emergTable(emerg) {
    var table = document.createElement('table');
    table.className = 'data';
    var head = ['before T0', 'speed', 'throttle', 'brake', 'accel. long',
                'belt', 'airbag lamp'];
    var thead = document.createElement('thead');
    var trh = document.createElement('tr');
    head.forEach(function (h) {
      var th = document.createElement('th');
      th.className = 'num';
      th.scope = 'col';
      th.textContent = h;
      trh.appendChild(th);
    });
    thead.appendChild(trh);
    table.appendChild(thead);
    var tbody = document.createElement('tbody');
    (emerg.points || []).forEach(function (pt) { tbody.appendChild(pointRow(pt)); });
    table.appendChild(tbody);
    return table;
  }

  var SEAL_CAUSE = ['normal end of event', 'power cut during the event',
                    'supply voltage drop', 'recycled for lack of space'];

  function sealList(seal) {
    var ul = document.createElement('ul');
    ul.className = 'emu-key';
    [['sealed', seal.file_complete ? 'file complete' : 'file INCOMPLETE'],
     ['cause', SEAL_CAUSE[seal.cause] || seal.cause],
     ['max Δv long', (seal.max_dv_long_mm_s / 1000).toFixed(2) + ' m/s'],
     ['max Δv lat', (seal.max_dv_lat_mm_s / 1000).toFixed(2) + ' m/s'],
     ['event duration', seal.time_since_first_ms + ' ms']].forEach(function (row) {
      var li = document.createElement('li');
      li.textContent = row[0] + ': ' + row[1];
      ul.appendChild(li);
    });
    return ul;
  }

  function renderDisks(res) {
    var total = SEGMENTS * 4096;
    var before = Object.assign({ total: total }, res.image || {});
    var after = Object.assign({ total: total }, res.after || {});
    window.AmbreDraw.drawDisk($('emu-disk-before'), before,
                              'at the cut — what the power loss left on the platter');
    window.AmbreDraw.drawDisk($('emu-disk-after'), after,
                              'after recovery — what edr-journal rebuilt at boot');
  }

  /* --------------------------------------------------------------- sweep -- */

  function bindSweep() {
    $('emu-sweep-run').addEventListener('click', function () {
      var mode = $('emu-sweep-mode').value;
      startSweep(mode === 'full' ? 0 : 2000, SWEEP_MASKS[mode] || SWEEP_MASKS.quick);
    });
  }

  function startSweep(maxStates, mask) {
    var plan = ask('web_sweep_start', ['number', 'number'], [maxStates, mask]);
    var btn = $('emu-sweep-run');
    btn.disabled = true;
    setText('emu-sweep-note', 'Exploring ' + fmt(plan.total) + ' power-cut states ' +
            'with the real recovery and the real invariant checker…');
    (function step() {
      var res = ask('web_sweep_step', ['number'], [SWEEP_BATCH]);
      paintSweep(res, plan.total);
      if (!res.finished) {
        window.setTimeout(step, 0);
      } else {
        btn.disabled = false;
        setText('emu-sweep-note', sweepNote(res));
      }
    }());
  }

  function sweepNote(res) {
    var rate = Math.round(res.states_per_second);
    return fmt(res.states_explored) + ' states explored in ' +
           res.seconds.toFixed(1) + ' s (' + fmt(rate) + ' states/s in this tab), ' +
           fmt(res.violations) + ' violation' + (res.violations === 1 ? '' : 's') +
           '. The PC campaign of record explored 1 379 264 states over 18 ' +
           'scenarios with 0 violations — see the validation page.';
  }

  function paintSweep(res, total) {
    var pct = total ? Math.min(100, (res.done / total) * 100) : 0;
    var bar = $('emu-sweep-bar');
    if (bar) bar.style.width = pct.toFixed(1) + '%';
    var body = $('emu-sweep-counters');
    var rows = [['states explored', res.states_explored],
                ['recoveries run', res.recoveries_run],
                ['violations', res.violations],
                ['acquired-event observations', res.events_acquired],
                ['phantom events', res.events_phantom]];
    body.textContent = '';
    rows.forEach(function (r) {
      var tr = document.createElement('tr');
      var th = document.createElement('th');
      var td = document.createElement('td');
      th.scope = 'row';
      th.textContent = r[0];
      td.className = 'num';
      td.textContent = fmt(r[1]);
      tr.appendChild(th);
      tr.appendChild(td);
      body.appendChild(tr);
    });
  }

  document.addEventListener('DOMContentLoaded', boot);
}());
