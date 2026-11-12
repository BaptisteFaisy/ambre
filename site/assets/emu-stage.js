/*
 * emu-stage.js — the animated stage: 12 V rail, road, instruments, banner.
 *
 * A pure renderer: it draws the view object the controller hands it and holds
 * no state of its own. Colours come from the site stylesheet so the stage
 * matches the rest of the pages; the background stays dark on purpose, it is
 * an instrument screen, not a page section.
 */
window.AmbreStage = (function () {
  'use strict';

  var RAIL_H = 46, ROAD_H = 274, STRIP_TOP = 330, STRIP_H = 104;
  var BANNER_TOP = 446, BANNER_H = 58, LAMP_TOP = 512, LAMP_H = 34;
  var ROAD_W = 0.5;

  /* The stage is an instrument screen: it always draws dark, with its own
   * palette (the site's dark theme), so it stays readable in both themes. */
  function pal() {
    return { amber: '#f2a33c', teal: '#46c3b2', red: '#ef6b6b',
             green: '#62d188', ink: '#e8edf4', dim: '#8b97a8',
             line: '#222c3a' };
  }

  function mono(px) { return px + 'px ui-monospace, SFMono-Regular, Menlo, monospace'; }

  function prepare(canvas) {
    var dpr = window.devicePixelRatio || 1;
    var w = canvas.clientWidth || canvas.parentNode.clientWidth || 960;
    var h = LAMP_TOP + LAMP_H + 6;
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    canvas.style.height = h + 'px';
    var ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx: ctx, w: w, h: h, pal: pal() };
  }

  /* --------------------------------------------------------------- rail -- */

  function drawRail(g, v) {
    var c = g.ctx, p = g.pal, y = 26, x1 = 16, x2 = g.w - 16;
    c.fillStyle = '#0a0e14';
    c.fillRect(0, 0, g.w, RAIL_H);
    c.font = mono(11);
    c.fillStyle = p.dim;
    c.fillText('12 V RAIL', x1, 15);
    c.textAlign = 'right';
    c.fillStyle = v.power.v > 6 ? p.green : p.red;
    c.fillText(v.power.v.toFixed(1) + ' V', x2, 15);
    c.textAlign = 'left';
    c.lineWidth = 3;
    c.strokeStyle = v.power.v > 6 ? p.green : p.red;
    c.beginPath();
    c.moveTo(x1, y);
    if (v.power.broken) {
      c.lineTo(v.power.breakX, y);
      c.stroke();
      c.beginPath();
      c.moveTo(v.power.breakX + 10, y + 26);
      c.lineTo(x2, y + 26);
    } else {
      c.lineTo(x2, y);
    }
    c.stroke();
    if (v.power.spark > 0) {
      c.fillStyle = 'rgba(255,220,140,' + v.power.spark.toFixed(2) + ')';
      c.beginPath();
      c.arc(v.power.breakX, y, 5 + 14 * v.power.spark, 0, 6.284);
      c.fill();
    }
  }

  /* --------------------------------------------------------------- road -- */

  function drawCar(c, cx, cy, brake, shake) {
    c.save();
    c.translate(cx + shake.x, cy + shake.y);
    c.fillStyle = '#2b3546';
    roundRect(c, -46, -18, 92, 40, 8);
    c.fill();
    c.fillStyle = '#1b2230';
    roundRect(c, -30, -34, 60, 20, 6);
    c.fill();
    c.fillStyle = brake ? '#ff5b5b' : '#7a2b2b';
    c.fillRect(-42, -6, 14, 6);
    c.fillRect(28, -6, 14, 6);
    c.fillStyle = '#0d1117';
    c.fillRect(-50, 16, 16, 8);
    c.fillRect(34, 16, 16, 8);
    c.restore();
  }

  function roundRect(c, x, y, w, h, r) {
    c.beginPath();
    c.moveTo(x + r, y);
    c.arcTo(x + w, y, x + w, y + h, r);
    c.arcTo(x + w, y + h, x, y + h, r);
    c.arcTo(x, y + h, x, y, r);
    c.arcTo(x, y, x + w, y, r);
    c.closePath();
  }

  function drawRoad(g, v) {
    var c = g.ctx, p = g.pal, w = g.w * ROAD_W, y0 = RAIL_H, h = ROAD_H;
    var hz = y0 + h * 0.44, car = v.frame.car;
    var sky = c.createLinearGradient(0, y0, 0, hz);
    sky.addColorStop(0, '#0b1018');
    sky.addColorStop(1, '#151d29');
    c.fillStyle = sky;
    c.fillRect(0, y0, w, h);
    c.fillStyle = '#1d2532';
    c.beginPath();
    c.moveTo(w * 0.45, hz);
    c.lineTo(w * 0.55, hz);
    c.lineTo(w * 0.97, y0 + h);
    c.lineTo(w * 0.03, y0 + h);
    c.closePath();
    c.fill();
    drawLanes(c, w, hz, y0 + h, v.scroll);
    drawCar(c, w * 0.5, y0 + h - 52, car.brake, v.shake);
    if (v.flash > 0) {
      var gr = c.createRadialGradient(w * 0.5, hz + 20, 4, w * 0.5, hz + 20, w * 0.6);
      gr.addColorStop(0, 'rgba(255,236,190,' + (0.85 * v.flash).toFixed(2) + ')');
      gr.addColorStop(1, 'rgba(255,150,60,0)');
      c.fillStyle = gr;
      c.fillRect(0, y0, w, h);
    }
    c.font = mono(11);
    c.fillStyle = p.dim;
    c.fillText('injected scenario — ' + (v.driveName || 'synthetic drive'), 12, y0 + 16);
  }

  function drawLanes(c, w, hz, bottom, scroll) {
    c.fillStyle = '#c8d2e0';
    for (var i = 0; i < 9; i++) {
      var t = ((i + scroll) % 9) / 9;
      var y = hz + (bottom - hz) * t * t;
      var halfW = 2 + 26 * t * t;
      c.globalAlpha = 0.25 + 0.6 * t;
      c.fillRect(w * 0.5 - halfW / 2, y, halfW, 3 + 16 * t);
    }
    c.globalAlpha = 1;
  }

  /* -------------------------------------------------------- instruments -- */

  function drawGauge(g, v, x, y, r) {
    var c = g.ctx, p = g.pal, speed = v.frame.car.speed || 0;
    var a0 = Math.PI * 0.75, a1 = Math.PI * 2.25;
    c.lineWidth = 7;
    c.strokeStyle = '#222c3a';
    c.beginPath(); c.arc(x, y, r, a0, a1); c.stroke();
    var k = Math.max(0, Math.min(1, speed / 140));
    c.strokeStyle = p.amber;
    c.beginPath(); c.arc(x, y, r, a0, a0 + (a1 - a0) * k); c.stroke();
    c.save();
    c.translate(x, y);
    c.rotate(a0 + (a1 - a0) * k);
    c.strokeStyle = p.ink;
    c.lineWidth = 2;
    c.beginPath(); c.moveTo(0, 0); c.lineTo(r - 6, 0); c.stroke();
    c.restore();
    c.font = mono(22);
    c.fillStyle = p.ink;
    c.textAlign = 'center';
    c.fillText(speed.toFixed(1), x, y + 8);
    c.font = mono(10);
    c.fillStyle = p.dim;
    c.fillText('km/h', x, y + 22);
    c.fillText('class A, 2 Hz', x, y + 46);
    c.textAlign = 'left';
  }

  function drawAccel(g, v, x, y, w, h) {
    var c = g.ctx, p = g.pal, a = v.frame.accel || [];
    c.strokeStyle = '#222c3a';
    c.lineWidth = 1;
    c.strokeRect(x, y, w, h);
    if (a.length < 2) return;
    var max = 1;
    a.forEach(function (v2) { max = Math.max(max, Math.abs(v2)); });
    c.beginPath();
    for (var i = 0; i < a.length; i++) {
      var px = x + (w * i) / (a.length - 1);
      var py = y + h / 2 - (a[i] / max) * (h / 2 - 3);
      if (i === 0) c.moveTo(px, py); else c.lineTo(px, py);
    }
    c.strokeStyle = p.teal;
    c.lineWidth = 1.6;
    c.stroke();
    c.strokeStyle = '#2c3746';
    c.beginPath(); c.moveTo(x, y + h / 2); c.lineTo(x + w, y + h / 2); c.stroke();
    c.font = mono(10);
    c.fillStyle = p.dim;
    c.fillText('a_long, last 1.5 s  ·  peak ' + (max / 1000).toFixed(0) + ' m/s²', x + 4, y + 12);
  }

  function drawRing(g, v, x, y, w) {
    var c = g.ctx, p = g.pal, ring = v.frame.ring || [];
    var n = 11, cw = w / n;
    c.font = mono(10);
    c.fillStyle = p.dim;
    c.fillText('class A ring buffer — 5 s at 2 Hz, kept in RAM until the trigger', x, y - 5);
    for (var i = 0; i < n; i++) {
      var pt = ring[i];
      c.fillStyle = pt ? p.amber : '#1d2532';
      c.globalAlpha = pt ? 0.35 + 0.65 * (1 - i / n) : 1;
      roundRect(c, x + i * cw + 2, y, cw - 4, 26, 3);
      c.fill();
      c.globalAlpha = 1;
      if (pt) {
        c.fillStyle = '#0a0e14';
        c.font = mono(9);
        c.fillText(pt.speed.toFixed(0), x + i * cw + 5, y + 12);
        c.fillText('-' + (pt.age / 1000).toFixed(1) + 's', x + i * cw + 5, y + 22);
      }
    }
  }

  /* The freeze window: how many milliseconds since T0, against the deadline. */
  function drawWindow(g, v, x, y, w) {
    var c = g.ctx, p = g.pal, age = v.frame.impactAge_ms;
    if (age === null || age === undefined || !v.frame.t0) return;
    var span = 120, k = Math.max(0, Math.min(1, (age + 20) / span));
    c.font = mono(10);
    c.fillStyle = p.dim;
    c.fillText('the window: impact at T0, the 12 V rail dies tens of ms later', x, y - 5);
    c.fillStyle = '#1d2532';
    roundRect(c, x, y, w, 14, 7); c.fill();
    c.fillStyle = age < 0 ? p.dim : (v.power.v > 6 ? p.amber : p.red);
    roundRect(c, x, y, Math.max(3, w * k), 14, 7); c.fill();
    var tx = x + w * (20 / span);
    c.strokeStyle = p.ink;
    c.lineWidth = 2;
    c.beginPath(); c.moveTo(tx, y - 4); c.lineTo(tx, y + 18); c.stroke();
    c.font = mono(10);
    c.fillStyle = p.ink;
    c.fillText('T0', tx + 4, y + 28);
    c.textAlign = 'right';
    c.fillStyle = age >= 0 ? p.red : p.dim;
    c.fillText((age >= 0 ? 'T0 +' : 'T0 ') + age.toFixed(1) + ' ms', x + w, y - 5);
    c.textAlign = 'left';
  }

  /* ------------------------------------------------------------- banner -- */

  function drawBanner(g, v) {
    var c = g.ctx, p = g.pal, f = v.frame;
    c.fillStyle = '#0a0e14';
    c.fillRect(0, BANNER_TOP, g.w, g.h - BANNER_TOP);
    c.strokeStyle = '#1c2532';
    c.beginPath(); c.moveTo(0, BANNER_TOP); c.lineTo(g.w, BANNER_TOP); c.stroke();
    c.font = '600 15px ' + 'ui-sans-serif, system-ui, sans-serif';
    c.fillStyle = p.ink;
    c.fillText(f.banner.title, 16, BANNER_TOP + 22);
    c.font = mono(11);
    c.fillStyle = p.dim;
    var right = 'op ' + f.op + ' / ' + f.opTotal + '   ·   t = ' +
                (f.t_us / 1000).toFixed(1) + ' ms';
    if (f.banner.ref) right = f.banner.ref + '   ·   ' + right;
    c.textAlign = 'right';
    c.fillText(right, g.w - 16, BANNER_TOP + 22);
    c.textAlign = 'left';
    if (f.slowmo) {
      c.fillStyle = p.amber;
      c.fillText('slow motion ×' + Math.round(f.slowmo) +
                 ' — the freeze window, 120 ms of real time', 16, BANNER_TOP + 40);
    } else {
      c.fillStyle = p.dim;
      c.fillText(v.bannerNote || 'the drive, compressed: the journal writes continuously',
                 16, BANNER_TOP + 40);
    }
    c.fillStyle = '#1c2532';
    c.fillRect(16, BANNER_TOP + 48, g.w - 32, 4);
    c.fillStyle = p.amber;
    c.fillRect(16, BANNER_TOP + 48, (g.w - 32) * (f.op / Math.max(1, f.cut)), 4);
  }

  /* ------------------------------------------------------------ overlay -- */

  function drawOverlay(g, v) {
    var c = g.ctx, p = g.pal, o = v.overlay;
    if (o.dim > 0) {
      c.fillStyle = 'rgba(4,6,9,' + o.dim.toFixed(2) + ')';
      c.fillRect(0, 0, g.w, g.h);
    }
    if (o.title) {
      c.font = '700 30px ui-sans-serif, system-ui, sans-serif';
      c.textAlign = 'center';
      c.fillStyle = o.colour || p.red;
      c.fillText(o.title, g.w * 0.26, 185);
      if (o.sub) {
        c.font = mono(12);
        c.fillStyle = p.ink;
        c.fillText(o.sub, g.w * 0.26, 210);
      }
      c.textAlign = 'left';
    }
    if (o.flash > 0) {
      c.fillStyle = 'rgba(255,255,255,' + (0.5 * o.flash).toFixed(2) + ')';
      c.fillRect(0, 0, g.w, g.h);
    }
  }

  function draw(g, v) {
    var c = g.ctx;
    c.save();
    c.translate(v.shake.x, v.shake.y);
    c.fillStyle = '#070a0f';
    c.fillRect(-8, -8, g.w + 16, g.h + 16);
    drawRoad(g, v);
    drawInstruments(g, v);
    c.restore();
    drawRail(g, v);
    window.AmbreStrip.drawStrip(g, v);
    drawBanner(g, v);
    window.AmbreStrip.drawLamps(g, v);
    drawOverlay(g, v);
  }

  function drawInstruments(g, v) {
    var x = g.w * ROAD_W + 16, w = g.w - x - 16, y0 = RAIL_H;
    var c = g.ctx;
    c.fillStyle = '#0a0e14';
    c.fillRect(g.w * ROAD_W, y0, g.w * (1 - ROAD_W), ROAD_H);
    drawGauge(g, v, x + 62, y0 + 74, 46);
    drawAccel(g, v, x + 128, y0 + 26, w - 128, 96);
    drawRing(g, v, x, y0 + 160, w);
    drawWindow(g, v, x, y0 + 216, w);
  }

  return { prepare: prepare, draw: draw, geom: { rail: RAIL_H, stripTop: STRIP_TOP,
           stripH: STRIP_H, lampTop: LAMP_TOP } };
}());
