/*
 * emulator.js — the controller of the animated stage on emulator.html.
 *
 * It owns no journal logic: the C module (host/web/, compiled to WebAssembly)
 * supplies the op trace, the injected drive, the ground truth and the verdict
 * of a cut. This file loads that module, sequences the animation, and draws.
 */
(function () {
  'use strict';

  var SCENARIO_URL = 'assets/data/scenario-web.txt';
  var SCENARIO_PATH = '/scenario-web.txt';
  var SEGMENTS = 2, SEED = 1234;
  var OPS_SLICE = 1200, SAMPLE_SLICE = 1500, SWEEP_BATCH = 120;
  var CRC_CHECK = 0xE3069283;
  var SWEEP_MASKS = { quick: 0x1E, full: 0x3E };
  /* Phases after the trace has been played: the cut, the dark, the reboot,
   * the recovery scan, the verdict. Durations in animation seconds. */
  var PHASES = ['play', 'cut', 'dark', 'boot', 'recover', 'verdict'];
  var PHASE_S = { cut: 0.5, dark: 1.0, boot: 1.1, recover: 2.8, verdict: 1.8 };
  var BANNER_NOTE = {
    play: '',
    cut: 'nothing written after the cut reaches the media',
    dark: 'the machine is off; the platter is all that is left',
    boot: 'recovery runs at boot, before any service starts',
    recover: 'the scan reads every block of the used area, in order',
    verdict: 'the eight invariants, checked against the injected truth'
  };

  var S = {
    mod: null, info: null, trace: null, ops: [], samples: [], drive: [],
    player: null, state: null, crashCodes: null, afterCodes: null,
    phase: 'play', t: 0, phaseT: 0, speed: 1, playing: false, last: 0,
    scroll: 0, shake: { x: 0, y: 0 }, flash: 0, spark: 0,
    lamps: [0, 0, 0, 0, 0, 0, 0, 0], g: null, cut: 0, total: SEGMENTS * 4096
  };

  function $(id) { return document.getElementById(id); }
  var P = window.AmbrePanels;

  /* ------------------------------------------------------------- loading -- */

  function call(name, ret, types, args) {
    return S.mod.ccall(name, ret, types || [], args || []);
  }

  function ask(name, types, args) {
    return JSON.parse(call(name, 'string', types, args));
  }

  function slices(name, total, size) {
    var out = [], key = name === 'web_ops' ? 'ops' : 'samples';
    for (var from = 0; from < total; from += size) {
      ask(name, ['number', 'number'], [from, size])[key].forEach(function (x) {
        out.push(x);
      });
    }
    return out;
  }

  function selfTest() {
    var crc = call('web_crc_selftest', 'number', [], []) >>> 0;
    if (crc !== CRC_CHECK) {
      throw new Error('CRC-32C self-test gave 0x' + crc.toString(16));
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
    return text ? SCENARIO_URL : 'synthetic drive built in C (seed ' + SEED + ')';
  }

  function boot() {
    setStatus('busy', 'Loading the compiled explorer…', 'site/assets/ambre-emu.js');
    fetch(SCENARIO_URL).then(function (r) {
      return r.ok ? r.text() : null;
    }).catch(function () { return null; }).then(function (text) {
      return AmbreEmu().then(function (mod) {
        S.mod = mod;
        selfTest();
        var source = setup(text);
        S.info = ask('web_info', [], []);
        S.trace = ask('web_trace', [], []);
        S.ops = slices('web_ops', S.trace.op_count, OPS_SLICE);
        S.samples = slices('web_samples', S.info.sample_count / 2, SAMPLE_SLICE);
        S.drive = window.AmbrePlay.parseScenario(text);
        S.driveName = source;
        afterBoot();
      });
    }).catch(function (err) {
      setStatus('error', 'The emulator did not start.', String(err && err.message || err));
    });
  }

  function afterBoot() {
    S.g = window.AmbreStage.prepare($('emu-stage'));
    renderStatic();
    bindControls();
    setCut(defaultCut());
    fetchState();
    S.playing = true;
    S.last = performance.now();
    requestAnimationFrame(tick);
    var q = new URLSearchParams(location.search);
    if (q.has('t')) {
      S.playing = false;
      seekTo(Number(q.get('t')) || 0);
      $('emu-play').textContent = 'Play';
    }
    autoSweep();
  }

  function defaultCut() {
    var ev = (S.info.events || [])[0];
    if (ev && ev.acquired_op >= 0) return ev.acquired_op + 6;
    return Math.floor(S.trace.op_count / 2);
  }

  function setStatus(state, message, detail) {
    var box = $('emu-status');
    if (box) box.setAttribute('data-state', state);
    P.setText('emu-status-msg', message);
    P.setText('emu-status-detail', detail || '');
  }

  function renderStatic() {
    setStatus('ok', 'Ready — the real explorer is running in this tab.',
              P.fmt(S.trace.op_count) + ' ops · ' + P.fmt(S.trace.writes) +
              ' writes · ' + P.fmt(S.info.events.length) + ' events · CRC-32C ok');
    P.setText('emu-drive', S.driveName);
    P.setText('emu-trace-summary',
              P.fmt(S.trace.writes) + ' writes, ' + P.fmt(S.trace.flushes) +
              ' flushes, ' + P.fmt(S.trace.marks) + ' durability marks; longest single write ' +
              P.fmt(S.trace.longest_write) + ' B; ' + P.fmt(S.trace.bytes_written) +
              ' B written in total.');
    P.setText('emu-events-truth', truthSummary());
    fetch('assets/ambre-emu.js.sha256').then(function (r) {
      return r.ok ? r.text() : '';
    }).then(function (sha) {
      if (sha) P.setText('emu-sha256', sha.trim());
    }).catch(function () { /* offline: the checksum stays hidden */ });
  }

  function truthSummary() {
    var ev = S.info.events || [];
    if (!ev.length) return 'This drive triggered no event.';
    var f = ev[0];
    return ev.length + ' events triggered by the impact pulse (the journal table holds ' +
           'at most 8). The first: EMERG written at op ' + f.emerg_op + ', acquired at op ' +
           f.acquired_op + ', sealed at op ' + f.sealed_op + (f.locked ? ' (locked)' : '') + '.';
  }

  /* --------------------------------------------------------- crash state -- */

  function variantParams() {
    var checked = document.querySelector('input[name="emu-variant"]:checked');
    var v = checked ? Number(checked.value) : 0;
    var p = { variant: v, torn: 0, lo: 0, hi: 0, off: 0, bit: 0, both: 0 };
    if (v === 1) p.torn = Number($('emu-torn').value) * 512;
    if (v === 2) {
      var mask = $('emu-mask').value;
      p.lo = mask === 'all' ? 0xFFFFFFFF : Number(mask);
      p.hi = mask === 'all' ? 0xFFFFFFFF : 0;
    }
    if (v === 3) { p.off = Number($('emu-offset').value); p.bit = Number($('emu-bit').value); }
    if (v === 4) p.both = $('emu-table-both').checked ? 1 : 0;
    return p;
  }

  /* The real answer of the explorer for the chosen cut and variant. */
  function fetchState() {
    var p = variantParams();
    S.state = ask('web_state',
      ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [S.cut, p.variant, p.torn, p.lo, p.hi, p.off, p.bit, p.both]);
    S.crashCodes = runsToCodes(S.state.image, S.total);
    S.afterCodes = runsToCodes(S.state.after, S.total);
    P.verdict(S.state);
    P.recovery(S.state);
    P.events(S.state);
    P.disks(S.state, SEGMENTS);
    showParams();
    restart();
  }

  function runsToCodes(image, total) {
    var codes = new Int8Array(total);
    ((image && image.runs) || []).forEach(function (r) {
      var start = r[0] * 4096 + r[1];
      for (var k = 0; k < r[2] && start + k < total; k++) codes[start + k] = r[3];
    });
    return codes;
  }

  function showParams() {
    var v = variantParams().variant;
    ['torn', 'cache', 'bitflip', 'table'].forEach(function (name, i) {
      var el = $('emu-params-' + name);
      if (el) el.hidden = (v !== i + 1);
    });
    P.setText('emu-variant-note', S.state ? S.state.note : '');
  }

  /* ------------------------------------------------------------ controls -- */

  function setCut(value) {
    S.cut = Math.max(1, Math.min(S.trace.op_count, value | 0));
    var slider = $('emu-cut');
    if (slider) slider.value = String(S.cut);
    P.setText('emu-cut-value', P.fmt(S.cut) + ' / ' + P.fmt(S.trace.op_count));
    S.player = new window.AmbrePlay.Player(
      { info: S.info, ops: S.ops, drive: S.drive, samples: S.samples,
        blocksTotal: S.total }, S.cut);
    describeCut();
    window.AmbreDraw.drawTimeline($('emu-timeline'), S.ops, S.cut);
  }

  function describeCut() {
    var where = 'before any event was triggered';
    (S.info.events || []).forEach(function (e) {
      if (S.cut > e.sealed_op && e.sealed_op >= 0) where = 'after event ' + e.event_id + ' was sealed';
      else if (S.cut > e.acquired_op && e.acquired_op >= 0) where = 'after event ' + e.event_id + ' was acquired (durable)';
      else if (S.cut > e.emerg_op && e.emerg_op >= 0) where = 'after the EMERG of event ' + e.event_id + ' was written, before the flush';
    });
    P.setText('emu-cut-where', where);
  }

  function restart() {
    S.phase = 'play';
    S.t = 0;
    S.phaseT = 0;
    S.flash = 0;
    S.spark = 0;
    S.shake = { x: 0, y: 0 };
    S.lamps = [0, 0, 0, 0, 0, 0, 0, 0];
    if (S.player) S.player.reset();
  }

  /* Jump to an animation time: the trace part, then the phases after the cut.
   * ?t= on the URL uses it, so any moment of the crash can be linked to. */
  function seekTo(t) {
    restart();
    if (t <= S.player.duration) {
      S.t = t;
      S.phase = 'play';
      return;
    }
    var left = t - S.player.duration;
    S.t = S.player.duration;
    for (var i = 1; i < PHASES.length; i++) {
      var d = PHASE_S[PHASES[i]];
      if (left <= d || i === PHASES.length - 1) {
        S.phase = PHASES[i];
        S.phaseT = Math.min(left, d);
        break;
      }
      left -= d;
    }
    if (S.phase === 'verdict') lightLamps();
  }

  function bindControls() {
    var slider = $('emu-cut');
    slider.max = String(S.trace.op_count);
    slider.addEventListener('input', function () { setCut(Number(slider.value)); });
    slider.addEventListener('change', fetchState);
    $('emu-run').addEventListener('click', fetchState);
    $('emu-play').addEventListener('click', function () {
      S.playing = !S.playing;
      this.textContent = S.playing ? 'Pause' : 'Play';
    });
    $('emu-restart').addEventListener('click', function () {
      restart();
      S.playing = true;
      $('emu-play').textContent = 'Pause';
    });
    $('emu-speed').addEventListener('change', function () {
      S.speed = Number(this.value);
    });
    document.querySelectorAll('input[name="emu-variant"]').forEach(function (r) {
      r.addEventListener('change', function () { showParams(); fetchState(); });
    });
    ['emu-torn', 'emu-offset', 'emu-bit', 'emu-mask', 'emu-table-both'].forEach(function (id) {
      var el = $(id);
      if (el) el.addEventListener('change', fetchState);
    });
    $('emu-offset').max = String(S.info.disk_size - 1);
    bindPresets();
    $('emu-sweep-run').addEventListener('click', function () {
      var mode = $('emu-sweep-mode').value;
      startSweep(mode === 'full' ? 0 : 2000, SWEEP_MASKS[mode]);
    });
  }

  function bindPresets() {
    var ev = (S.info.events || [])[0] || {};
    var targets = { start: 1, emerg: (ev.emerg_op || 0) + 1,
                    acquired: (ev.acquired_op || 0) + 1,
                    sealed: (ev.sealed_op || 0) + 1, end: S.trace.op_count };
    document.querySelectorAll('[data-cut]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        setCut(targets[btn.getAttribute('data-cut')] || 1);
        fetchState();
      });
    });
  }

  function autoSweep() {
    var mode = new URLSearchParams(location.search).get('sweep');
    if (mode !== 'quick' && mode !== 'full') return;
    var sel = $('emu-sweep-mode');
    if (sel) sel.value = mode;
    startSweep(mode === 'full' ? 0 : 2000, SWEEP_MASKS[mode]);
  }

  function startSweep(maxStates, mask) {
    var plan = ask('web_sweep_start', ['number', 'number'], [maxStates, mask]);
    var btn = $('emu-sweep-run');
    btn.disabled = true;
    P.setText('emu-sweep-note', 'Exploring ' + P.fmt(plan.total) +
              ' power-cut states with the real recovery and the real checker…');
    (function step() {
      var res = ask('web_sweep_step', ['number'], [SWEEP_BATCH]);
      P.sweep(res, plan.total);
      if (!res.finished) {
        window.setTimeout(step, 0);
      } else {
        btn.disabled = false;
        P.setText('emu-sweep-note', P.sweepNote(res));
      }
    }());
  }

  /* ---------------------------------------------------------- animation -- */

  function advance(dt) {
    if (S.phase === 'play') {
      S.t += dt;
      if (S.t >= S.player.duration) { S.t = S.player.duration; nextPhase(); }
      return;
    }
    S.phaseT += dt;
    if (S.phaseT >= PHASE_S[S.phase]) nextPhase();
  }

  function nextPhase() {
    var i = PHASES.indexOf(S.phase);
    S.phase = PHASES[Math.min(PHASES.length - 1, i + 1)];
    S.phaseT = 0;
    if (S.phase === 'verdict') lightLamps();
  }

  function lightLamps() {
    var bad = {};
    ((S.state && S.state.violations) || []).forEach(function (v) { bad[v] = 1; });
    S.lamps = P.INVARIANTS.map(function (name) {
      if (S.state.variant === 'bitflip' && P.BITFLIP_NA[name]) return 0;
      return bad[name] ? -1 : 1;
    });
  }

  function impactFx(frame, dt) {
    var age = frame.impactAge_ms;
    var hitting = age !== null && age > -2 && age < 45;
    S.flash = hitting ? Math.min(1, S.flash + dt * 6) : Math.max(0, S.flash - dt * 2.4);
    var amp = hitting ? Math.max(0, 5 - age / 10) : 0;
    S.shake = { x: (Math.random() - 0.5) * amp, y: (Math.random() - 0.5) * amp };
    S.scroll = (S.scroll + (frame.car.speed || 0) * dt * 0.06) % 9;
  }

  function powerOf() {
    if (S.phase === 'play') return { v: 12.0, broken: false, spark: 0, breakX: 0 };
    var k = Math.min(1, S.phaseT / PHASE_S.cut);
    var v = S.phase === 'cut' ? 12.0 * (1 - k) : 0;
    S.spark = S.phase === 'cut' ? Math.max(0, 1 - k * 1.6) : Math.max(0, S.spark - 0.04);
    return { v: v, broken: S.phase !== 'play', spark: S.spark,
             breakX: S.g.w * 0.62 };
  }

  function stripOf(frame) {
    if (S.phase === 'play' || !S.state) {
      return { codes: frame.strip.blocks, head: frame.strip.head,
               tables: frame.strip.tables, scan: null, after: null };
    }
    var img = S.state.image, scan = null;
    if (S.phase === 'recover') scan = Math.min(1, S.phaseT / PHASE_S.recover);
    if (S.phase === 'verdict') scan = 1;
    var tables = scan !== null && scan > 0.5
      ? { a: S.state.after.ta, b: S.state.after.tb, ga: S.state.after.ga, gb: S.state.after.gb }
      : { a: img.ta, b: img.tb, ga: img.ga, gb: img.gb };
    return { codes: S.crashCodes, head: -1, tables: tables, scan: scan,
             after: S.afterCodes };
  }

  function overlayOf(frame) {
    if (S.phase === 'cut') {
      return { dim: 0.15 + 0.5 * Math.min(1, S.phaseT / PHASE_S.cut), flash: 0,
               title: 'POWER LOST', sub: 'the 12 V rail is gone — everything not on the media is gone',
               colour: null, note: '' };
    }
    if (S.phase === 'dark') {
      return { dim: 0.82, flash: 0, title: '', sub: '', note: '' };
    }
    if (S.phase === 'boot') {
      return { dim: 0.55, flash: 0, title: 'REBOOT', colour: '#e8edf4',
               sub: 'edr-journal starts and runs recovery before anything else (REP-01)', note: '' };
    }
    if (S.phase === 'recover') {
      return { dim: 0.25, flash: 0, title: '', note: '',
               sub: 'scanning ' + P.fmt(S.state.recovery.scanned_blocks) + ' blocks…' };
    }
    if (S.phase === 'verdict') {
      var ok = S.state.ok;
      return { dim: 0.1, flash: 0, title: ok ? 'THE CRASH IS ON THE DISK' : 'INVARIANT VIOLATED',
               colour: ok ? '#62d188' : '#ef6b6b',
               sub: ok ? 'every acquired event came back byte-identical'
                       : (S.state.detail || ''), note: '' };
    }
    return { dim: 0, flash: 0, title: '', sub: '', note: '' };
  }

  function phaseBanner() {
    var r = (S.state && S.state.recovery) || {};
    switch (S.phase) {
      case 'cut':
        return { title: 'POWER LOST at op ' + S.cut +
                        ' — writes after it never reached the media', ref: '' };
      case 'dark':
        return { title: 'The rail is at 0 V: volatile caches are gone, the media keeps what it has', ref: '' };
      case 'boot':
        return { title: 'Reboot — edr-journal runs recovery before anything else', ref: 'REP-01' };
      case 'recover':
        return { title: 'Recovery: scan ' + P.fmt(r.scanned_blocks || 0) + ' blocks, drop ' +
                        P.fmt(r.torn_records || 0) + ' torn record(s), seal what was still open',
                 ref: 'REP-03' };
      case 'verdict':
        return { title: S.state.ok
                        ? 'Verdict: every acquired event came back byte-identical'
                        : 'Verdict: ' + (S.state.detail || 'violation'), ref: '' };
    }
    return { title: '', ref: '' };
  }

  function render() {
    if (!S.g || !S.player) return;
    var frame = S.player.frameAt(S.t);
    frame.t0 = S.player.t0;
    if (S.phase !== 'play') frame.banner = phaseBanner();
    var view = {
      frame: frame, driveName: S.driveName, scroll: S.scroll, shake: S.shake,
      flash: S.flash, power: powerOf(), strip: stripOf(frame),
      lamps: S.lamps, overlay: overlayOf(frame),
      bannerNote: BANNER_NOTE[S.phase] || '',
      lampNote: S.phase === 'verdict'
        ? (S.state.ok ? 'all eight hold for this cut' : 'see the detail below')
        : 'the eight invariants, checked against what was injected'
    };
    window.AmbreStage.draw(S.g, view);
  }

  function tick(now) {
    var dt = Math.min(0.05, (now - S.last) / 1000);
    S.last = now;
    if (S.playing) advance(dt * S.speed);
    render();
    requestAnimationFrame(tick);
  }

  window.addEventListener('resize', function () {
    if (S.player) S.g = window.AmbreStage.prepare($('emu-stage'));
  });

  document.addEventListener('DOMContentLoaded', boot);
}());
