/*
 * emu-panels.js — the written part of the emulator page: verdict, recovery
 * counters, what the black box holds, the two block maps, the sweep counters.
 * Everything it shows is a field of a JSON document the C module produced.
 */
window.AmbrePanels = (function () {
  'use strict';

  var INVARIANTS = ['I1', 'I2', 'I3', 'I4', 'I5', 'I6', 'I7', 'I8'];
  var INV_HINT = ['recovery terminates within its bounds',
                  'events acquired before the cut come back byte-identical',
                  'no event is invented',
                  'class B samples equal the injected ones',
                  'the journal is not stale',
                  'a locked event stays locked',
                  'the "complete file" flag tells the truth',
                  'a second recovery gives the same table'];
  /* Invariants the C checker skips for a media fault: a flipped bit has no
   * redundancy to repair it, so survival is not demanded of that state. */
  var BITFLIP_NA = { I2: 1, I4: 1, I5: 1, I6: 1 };
  var SEAL_CAUSE = ['normal end of event', 'power cut during the event',
                    'supply voltage drop', 'recycled for lack of space'];

  function $(id) { return document.getElementById(id); }

  function setText(id, value) {
    var el = $(id);
    if (el) el.textContent = value;
  }

  function fmt(n) {
    return typeof n === 'number' ? n.toLocaleString('en-US') : String(n);
  }

  function clear(id) {
    var el = $(id);
    if (el) el.textContent = '';
    return el;
  }

  function row(label, value) {
    var tr = document.createElement('tr');
    var th = document.createElement('th');
    var td = document.createElement('td');
    th.scope = 'row';
    th.textContent = label;
    td.className = 'num';
    td.textContent = typeof value === 'number' ? fmt(value) : String(value);
    tr.appendChild(th);
    tr.appendChild(td);
    return tr;
  }

  /* ------------------------------------------------------------- verdict -- */

  function verdict(res) {
    var list = clear('emu-verdict');
    var bad = {};
    (res.violations || []).forEach(function (v) { bad[v] = 1; });
    INVARIANTS.forEach(function (name, i) {
      var li = document.createElement('li');
      var na = res.variant === 'bitflip' && BITFLIP_NA[name];
      li.className = 'emu-inv';
      li.setAttribute('data-result', na ? 'na' : (bad[name] ? 'fail' : 'pass'));
      li.textContent = name + (na ? ' n/a' : (bad[name] ? ' ✗' : ' ✓'));
      li.title = INV_HINT[i];
      list.appendChild(li);
    });
    var detail = $('emu-detail');
    detail.textContent = res.ok
      ? 'No invariant violated. Recovery terminated, the acquired event came back ' +
        'byte-identical, nothing was invented, and a second recovery gave the same table.'
      : (res.detail || 'violation');
    detail.setAttribute('data-bad', res.ok ? '0' : '1');
    setText('emu-verdict-headline', res.ok ? 'Survives this cut'
            : 'VIOLATION — ' + (res.violations || []).join(', '));
    setText('emu-acquired', fmt(res.acquired_before_cut));
  }

  function recovery(res) {
    var body = clear('emu-recovery');
    var r = res.recovery || {};
    [['blocks scanned', r.scanned_blocks],
     ['events before / after', r.events_before + ' / ' + r.events_after],
     ['torn records dropped', r.torn_records],
     ['events sealed incomplete', r.sealed_incomplete],
     ['table commits', r.table_commits],
     ['table rebuilt from a scan', r.table_rebuilt ? 'yes' : 'no'],
     ['diagnostic', r.diag || '—']].forEach(function (pair) {
      body.appendChild(row(pair[0], pair[1]));
    });
  }

  /* ------------------------------------------------------------ black box - */

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

  function emergTable(emerg) {
    var table = document.createElement('table');
    table.className = 'data';
    var thead = document.createElement('thead');
    var trh = document.createElement('tr');
    ['before T0', 'speed', 'throttle', 'brake', 'accel. long', 'belt', 'airbag lamp']
      .forEach(function (h) {
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

  function sealList(seal) {
    var ul = document.createElement('ul');
    ul.className = 'emu-key';
    [['sealed', seal.file_complete ? 'file complete' : 'file INCOMPLETE'],
     ['cause', SEAL_CAUSE[seal.cause] || seal.cause],
     ['max Δv long', (seal.max_dv_long_mm_s / 1000).toFixed(2) + ' m/s'],
     ['max Δv lat', (seal.max_dv_lat_mm_s / 1000).toFixed(2) + ' m/s'],
     ['event duration', seal.time_since_first_ms + ' ms']].forEach(function (r) {
      var li = document.createElement('li');
      li.textContent = r[0] + ': ' + r[1];
      ul.appendChild(li);
    });
    return ul;
  }

  function eventCard(ev) {
    var panel = document.createElement('div');
    panel.className = 'emu-panel';
    var h = document.createElement('h3');
    h.textContent = 'Event ' + ev.event_id + ' — ' + (ev.state === 2 ? 'sealed' : 'open') +
      (ev.locked ? ', locked' : '') + ', file complete: ' +
      (ev.file_complete ? 'yes' : 'no');
    var p = document.createElement('p');
    p.className = 'emu-hint';
    p.textContent = 'read back with ambre_journal_read_event(): ' +
      (ev.have_emerg ? 'EMERG present' : 'EMERG missing') + ', ' + ev.cont_count +
      ' continuation records, ' + ev.data_blocks + ' class B blocks, trigger reason ' +
      ev.trig_reason + ', blocks ' + ev.start_block + '–' + ev.end_block +
      ' of segment ' + ev.start_seg + '.';
    panel.appendChild(h);
    panel.appendChild(p);
    if (ev.emerg) panel.appendChild(emergTable(ev.emerg));
    if (ev.seal) panel.appendChild(sealList(ev.seal));
    return panel;
  }

  function events(res) {
    var box = clear('emu-events');
    var list = res.events || [];
    if (!list.length) {
      var p = document.createElement('p');
      p.className = 'emu-empty';
      p.textContent = 'Nothing on the journal yet at this cut point: the recorder holds ' +
                      'no event, and the recovery correctly finds none.';
      box.appendChild(p);
      return;
    }
    list.forEach(function (ev) { box.appendChild(eventCard(ev)); });
  }

  /* ----------------------------------------------------------- block maps - */

  function disks(res, segments) {
    var total = segments * 4096;
    var before = Object.assign({ total: total }, res.image || {});
    var after = Object.assign({ total: total }, res.after || {});
    window.AmbreDraw.drawDisk($('emu-disk-before'), before,
                              'at the cut — what the power loss left on the platter');
    window.AmbreDraw.drawDisk($('emu-disk-after'), after,
                              'after recovery — what edr-journal rebuilt at boot');
  }

  /* ---------------------------------------------------------------- sweep - */

  function sweep(res, total) {
    var pct = total ? Math.min(100, (res.done / total) * 100) : 0;
    var bar = $('emu-sweep-bar');
    if (bar) bar.style.width = pct.toFixed(1) + '%';
    var body = clear('emu-sweep-counters');
    [['states explored', res.states_explored],
     ['recoveries run', res.recoveries_run],
     ['violations', res.violations],
     ['acquired-event observations', res.events_acquired],
     ['phantom events', res.events_phantom]].forEach(function (pair) {
      body.appendChild(row(pair[0], pair[1]));
    });
  }

  function sweepNote(res) {
    return fmt(res.states_explored) + ' states explored in ' + res.seconds.toFixed(1) +
      ' s (' + fmt(Math.round(res.states_per_second)) + ' states/s in this tab), ' +
      fmt(res.violations) + ' violation' + (res.violations === 1 ? '' : 's') +
      '. The PC campaign of record explored 1 379 264 states over 18 scenarios ' +
      'with 0 violations — see the validation page.';
  }

  return { verdict: verdict, recovery: recovery, events: events, disks: disks,
           sweep: sweep, sweepNote: sweepNote, fmt: fmt, setText: setText,
           INVARIANTS: INVARIANTS, BITFLIP_NA: BITFLIP_NA };
}());
