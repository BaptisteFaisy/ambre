/*
 * emu-play.js — the playback model of the animated stage.
 *
 * Turns what the C module reported (op trace, injected scenario, ground truth,
 * crash state) into frames the renderer draws. No journal rule lives here: the
 * model only sequences real data — every op, every timestamp, every block code
 * comes from the module.
 *
 * The clock is deliberately non-linear and says so: the drive is compressed,
 * the freeze window around T0 is played in slow motion, because the whole
 * point of the project happens inside 20 ms of real time.
 */
window.AmbrePlay = (function () {
  'use strict';

  var WINDOW_US = 60000;      /* the freeze window shown in slow motion */
  var REC_NAMES = { 1: 'DATA', 2: 'EMERG', 3: 'CONT', 4: 'SEAL', 5: 'GAP', 9: 'TABLE' };

  /* ------------------------------------------------------------- parsing -- */

  /* The injected drive, straight from the scenario file: t_us a_long a_lat
   * speed_kmh_x4 throttle brake rpm steer_x2 ext_flags (docs/FORMAT.md §11). */
  function parseScenario(text) {
    var out = [];
    if (!text) return out;
    text.split('\n').forEach(function (line) {
      if (!line || line[0] === '#' || line.indexOf('=') >= 0) return;
      var f = line.trim().split(/\s+/);
      if (f.length < 9) return;
      out.push({ ts: +f[0], aLong: +f[1], aLat: +f[2], speed: +f[3] / 4,
                 throttle: +f[4], brake: +f[5] !== 0, rpm: +f[6],
                 steer: +f[7] / 2, ext: +f[8] });
    });
    return out;
  }

  /* The clock of every op: writes carry their block timestamp, flushes and
   * marks inherit the one before them. */
  function stampOps(ops) {
    var last = 0;
    ops.forEach(function (o) {
      if (o[0] === 1 && o.length > 3 && o[3] > 0) last = o[3];
      o.ts = last;
    });
    return ops;
  }

  /* -------------------------------------------------------------- player -- */

  function Player(data, cut) {
    this.info = data.info;
    this.ops = stampOps(data.ops);
    this.drive = data.drive;              /* parsed scenario (may be empty) */
    this.samples = data.samples || [];    /* injected class B, from the module */
    this.cut = Math.max(1, Math.min(this.ops.length, cut));
    this.totalBlocks = data.blocksTotal;
    this.blocks = new Uint8Array(this.totalBlocks);
    this.tables = { a: 0, b: 0, ga: 0, gb: 0 };
    this.applied = -1;
    this.t0 = firstT0(this.info);
    this.zones = this.buildZones();
    this.duration = this.zones.total;
    this.state = null;                    /* crash state answer, once known */
  }

  function firstT0(info) {
    var ev = (info.events || [])[0];
    if (ev && ev.t0_ts_us) return ev.t0_ts_us;
    return info.last_ts_us ? info.last_ts_us / 2 : 0;
  }

  /* Three zones of animation time, split around the freeze window. Their
   * lengths follow how many ops each zone holds, with the window weighted up
   * so it plays in slow motion; the whole run stays a few seconds long. */
  Player.prototype.buildZones = function () {
    var lo = this.t0 > WINDOW_US ? this.t0 - WINDOW_US : 0;
    var hi = this.t0 + WINDOW_US;
    var a = 0, b = 0, i;
    for (i = 0; i < this.cut; i++) {
      if (this.ops[i].ts < lo) a = i + 1;
      else if (this.ops[i].ts <= hi) b = i + 1;
    }
    if (b < a) b = a;
    var c = this.cut;
    var nb = b - a;
    /* The freeze window always gets the largest share of the animation time:
     * a handful of ops and ~120 ms of real time, played in slow motion. */
    var hasWindow = nb > 0;
    var total = Math.min(18, Math.max(5, 2 + this.cut / 120));
    var before = total * (hasWindow ? 0.40 : 0.7);
    var window = hasWindow ? total * 0.45 : 0;
    var after = total - before - window;
    var realWindow = Math.max(1, (opTs(this.ops, Math.min(b, c) - 1) -
                                  opTs(this.ops, Math.max(0, a - 1))) / 1000);
    return { a: a, b: b, c: c, total: total, before: before, window: window,
             after: after, slowmo: (window * 1000) / realWindow };
  };

  function opTs(ops, i) { return ops.length ? (ops[Math.max(0, Math.min(i, ops.length - 1))].ts || 0) : 0; }

  /* Animation time (s) -> op index. Linear inside each zone, so the freeze
   * window plays in slow motion and the frame says by how much. */
  Player.prototype.opAt = function (t) {
    var z = this.zones;
    if (t <= 0) return 0;
    if (t < z.before) return Math.round(z.a * t / Math.max(0.001, z.before));
    if (t < z.before + z.window) {
      var k = (t - z.before) / Math.max(0.001, z.window);
      return z.a + Math.round((z.b - z.a) * k);
    }
    var k2 = (t - z.before - z.window) / Math.max(0.001, z.after);
    return z.b + Math.round((z.c - z.b) * Math.min(1, k2));
  };

  Player.prototype.zoneOf = function (op) {
    if (op < this.zones.a) return 'drive';
    if (op < this.zones.b) return 'freeze';
    return 'after';
  };

  /* ------------------------------------------------- applying the trace -- */

  /* One op of the trace, applied to the picture of the partition. */
  Player.prototype.applyOp = function (o) {
    if (o[0] === 3) {                       /* durability mark */
      if (o[3] === 5) this.tables[this.lastTable === 0 ? 'ga' : 'gb'] = o[4];
      return;
    }
    if (o[0] !== 1) return;                 /* flush: nothing lands */
    var type = o.length > 4 ? o[4] : 0;
    if (o[1] < 1024) {                      /* one of the two table copies */
      this.lastTable = o[1] === 0 ? 0 : 1;
      this.tables[this.lastTable === 0 ? 'a' : 'b'] = 1;
      return;
    }
    var idx = (o[1] - 1024) / 512;
    var count = Math.max(1, Math.round(o[2] / 512));
    for (var k = 0; k < count && idx + k < this.totalBlocks; k++) {
      this.blocks[idx + k] = type || 1;
    }
    this.head = idx + count - 1;
  };

  Player.prototype.applyTo = function (n) {
    if (n < this.applied) { this.reset(); }
    while (this.applied < n - 1) {
      this.applied++;
      var o = this.ops[this.applied];
      if (!o) break;
      this.applyOp(o);
    }
  };

  Player.prototype.reset = function () {
    this.blocks = new Uint8Array(this.totalBlocks);
    this.tables = { a: 0, b: 0, ga: 0, gb: 0 };
    this.applied = -1;
    this.head = -1;
    this.lastTable = 0;
  };

  /* ------------------------------------------------------- injected data -- */

  /* The injected sample at time ts, from the scenario file when the page could
   * read it, otherwise from the class B stream the module recorded. */
  Player.prototype.carAt = function (ts) {
    var d = this.drive;
    if (!d.length) return this.carFromSamples(ts);
    var i = Math.max(0, Math.min(d.length - 1, Math.round(ts / 1000)));
    var s = d[i];
    return { speed: s.speed, aLong: s.aLong, aLat: s.aLat, brake: s.brake,
             throttle: s.throttle, steer: s.steer, rpm: s.rpm };
  };

  Player.prototype.carFromSamples = function (ts) {
    var s = this.samples, i = Math.max(0, Math.min(s.length - 1, Math.round(ts / 1000)));
    var ev = (this.info.events || [])[0] || {};
    var pts = ev.points || [];
    var speed = pts.length ? pts[0][1] : 0;
    pts.forEach(function (p) { if (p[0] >= 0) speed = p[1]; });
    return { speed: speed, aLong: s.length ? s[i][1] : 0,
             aLat: s.length ? s[i][2] : 0, brake: false, throttle: 0,
             steer: 0, rpm: 0 };
  };

  /* The class A ring as the recorder holds it: 5 s at 2 Hz (docs/FORMAT.md §6). */
  Player.prototype.ringAt = function (ts) {
    var out = [], d = this.drive, step = 500;
    if (!d.length) return out;
    for (var age = 0; age <= 5000; age += step) {
      var t = ts - age * 1000;
      if (t < 0) break;
      var s = d[Math.min(d.length - 1, Math.round(t / 1000))];
      out.push({ age: age, speed: s.speed, brake: s.brake, aLong: s.aLong });
    }
    return out;
  };

  /* The acceleration history for the curve, oldest first. */
  Player.prototype.accelWindow = function (ts, spanUs) {
    var out = [], d = this.drive, s0 = Math.max(0, Math.round((ts - spanUs) / 1000));
    var s1 = Math.max(0, Math.round(ts / 1000));
    var src = d.length ? d : this.samples;
    for (var i = s0; i <= s1 && i < src.length; i++) {
      out.push(d.length ? src[i].aLong : src[i][1]);
    }
    return out;
  };

  /* ------------------------------------------------------------ narrative - */

  var BANNER = [
    [1, 'Continuous capture — class B samples into the raw journal', 'GEL-07'],
    [2, 'IMPACT: the EMERG record goes out in ONE contiguous write', 'GEL-01'],
    [9, 'Allocation table committed: the older copy, generation + 1', 'STO-04'],
    [3, 'Continuation record — class A every 10 ms during the event', 'GEL-04'],
    [4, 'SEAL record — end of event, "complete file" flag', 'GEL-05'],
    [5, 'GAP recorded — the capture was interrupted', 'ACQ-06']
  ];

  Player.prototype.banner = function (op) {
    var o = this.ops[Math.max(0, op - 1)];
    if (!o) return { title: 'Journal idle', ref: '' };
    if (o[0] === 2) return { title: 'Flush acknowledged — this is the durability point', ref: 'GEL-02' };
    if (o[0] === 3) {
      if (o[3] === 1) return { title: 'EMERG record written (event ' + o[4] + ')', ref: 'GEL-01' };
      if (o[3] === 2) return { title: 'Event ' + o[4] + ' ACQUIRED — the crash is on the media', ref: 'GEL-02' };
      if (o[3] === 3) return { title: 'Event ' + o[4] + ' sealed (complete file = ' + o[5] + ')', ref: 'GEL-05' };
      if (o[3] === 5) return { title: 'Table committed, generation ' + o[4], ref: 'STO-04' };
      return { title: 'Data flushed up to sequence ' + o[4], ref: 'GEL-07' };
    }
    var type = o.length > 4 ? o[4] : 0;
    for (var i = 0; i < BANNER.length; i++) {
      if (BANNER[i][0] === type) return { title: BANNER[i][1], ref: BANNER[i][2] };
    }
    return { title: 'Write of ' + o[2] + ' bytes at offset ' + o[1], ref: '' };
  };

  /* One frame of the animation, at animation time t (seconds). */
  Player.prototype.frameAt = function (t) {
    var op = Math.max(0, Math.min(this.cut, this.opAt(t)));
    this.applyTo(op);
    var ts = opTs(this.ops, op - 1);
    var zone = this.zoneOf(op);
    return {
      phase: zone, op: op, opTotal: this.ops.length, cut: this.cut,
      t_us: ts, car: this.carAt(ts), ring: this.ringAt(ts),
      accel: this.accelWindow(ts, 1500000),
      impactAge_ms: this.t0 ? (ts - this.t0) / 1000 : null,
      slowmo: zone === 'freeze' ? this.zones.slowmo : 0,
      strip: { blocks: this.blocks, head: this.head || 0,
               total: this.totalBlocks, tables: this.tables },
      banner: this.banner(op)
    };
  };

  return { Player: Player, parseScenario: parseScenario, REC_NAMES: REC_NAMES,
           WINDOW_US: WINDOW_US };
}());
