/*
 * emu-strip.js — the raw partition as seen from the stage, and the invariant
 * lamps. Also a pure renderer: the codes it draws come either from the op
 * trace (what the journal wrote) or from the real crash / recovered images
 * the C module returned.
 */
window.AmbreStrip = (function () {
  'use strict';

  var TOP = 330, HEIGHT = 104, LAMP_TOP = 512;
  var CELLS = 240;                 /* blocks visible in the follow window */
  var COLOURS = { 0: '#141b26', 1: '#4a5a70', 2: '#ef6b6b', 3: '#f2a33c',
                  4: '#62d188', 5: '#46c3b2', 9: '#c9a227' };
  var BAD = '#ff3b3b';

  function mono(px) { return px + 'px ui-monospace, SFMono-Regular, Menlo, monospace'; }

  function windowOf(strip) {
    var n = strip.codes.length;
    var count = Math.min(n, CELLS);
    var head = strip.head < 0 ? 0 : strip.head;
    var start = Math.max(0, Math.min(n - count, head - count + 40));
    return { start: start, count: count };
  }

  function codeAt(strip, i, win) {
    if (strip.scan === null || strip.scan === undefined || !strip.after) {
      return strip.codes[i];
    }
    var k = (i - win.start) / Math.max(1, win.count);
    return k <= strip.scan ? strip.after[i] : strip.codes[i];
  }

  function drawChips(g, v, x, y, h) {
    var c = g.ctx, p = g.pal, t = v.strip.tables;
    [['A', t.a, t.ga], ['B', t.b, t.gb]].forEach(function (chip, i) {
      var cx = x + i * 46;
      c.fillStyle = chip[1] === 1 ? p.green : (chip[1] === -1 ? BAD : '#1b2432');
      roundRect(c, cx, y, 40, h, 4);
      c.fill();
      c.fillStyle = '#07111a';
      c.font = mono(11);
      c.fillText(chip[0], cx + 5, y + 15);
      c.font = mono(9);
      c.fillText(chip[2] ? 'g' + chip[2] : '—', cx + 5, y + h - 6);
    });
    c.fillStyle = p.dim;
    c.font = mono(9);
    c.fillText('allocation table, two alternating copies (STO-04)', x, y + h + 24);
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

  function drawCells(g, v, x, y, w, h) {
    var c = g.ctx, p = g.pal, strip = v.strip, win = windowOf(strip);
    var cw = w / win.count;
    for (var i = 0; i < win.count; i++) {
      var idx = win.start + i;
      var code = codeAt(strip, idx, win);
      c.fillStyle = code === -1 || code === 200 ? BAD : (COLOURS[code] || COLOURS[0]);
      c.fillRect(x + i * cw, y, Math.max(1, cw - 0.6), h);
    }
    if (strip.head >= win.start && strip.head < win.start + win.count) {
      var hx = x + (strip.head - win.start) * cw;
      c.fillStyle = '#ffffff';
      c.fillRect(hx + cw / 2 - 1, y - 6, 2, h + 12);
      c.font = mono(9);
      c.fillStyle = p.dim;
      c.fillText('write head · block ' + strip.head,
                 Math.max(x + 74, Math.min(hx - 40, x + w - 260)), y + h + 12);
    }
    if (strip.scan !== null && strip.scan !== undefined) {
      var sx = x + w * strip.scan;
      c.fillStyle = 'rgba(70,195,178,0.9)';
      c.fillRect(sx - 1, y - 4, 2, h + 8);
      c.font = mono(10);
      c.fillText('recovery scan', Math.min(sx + 6, x + w - 90), y + h + 12);
    }
    return win;
  }

  function drawStrip(g, v) {
    var c = g.ctx, p = g.pal;
    c.fillStyle = '#0a0e14';
    c.fillRect(0, TOP, g.w, HEIGHT);
    c.strokeStyle = '#1c2532';
    c.beginPath(); c.moveTo(0, TOP); c.lineTo(g.w, TOP); c.stroke();
    c.font = mono(10);
    c.fillStyle = p.dim;
    c.fillText('edr-journal → raw partition, 512-byte blocks, no file system', 16, TOP + 13);
    var y = TOP + 22, h = 46;
    drawChips(g, v, 16, y, h);
    var x = 116, w = g.w - x - 16;
    var win = drawCells(g, v, x, y, w, h);
    c.font = mono(9);
    c.fillStyle = p.dim;
    c.fillText('block ' + win.start, x, y + h + 12);
    c.textAlign = 'right';
    c.fillText('block ' + (win.start + win.count - 1) + ' of ' + v.strip.codes.length,
               x + w, y + h + 12);
    c.textAlign = 'left';
    drawLegend(g, x + w - 430, TOP + 13);
  }

  function drawLegend(g, x, y) {
    var c = g.ctx, p = g.pal;
    var items = [[1, 'DATA'], [2, 'EMERG'], [3, 'CONT'], [4, 'SEAL'], [-1, 'invalid']];
    c.font = mono(9);
    items.forEach(function (it, i) {
      var cx = x + i * 86;
      c.fillStyle = it[0] === -1 ? BAD : (COLOURS[it[0]] || COLOURS[0]);
      c.fillRect(cx, y - 7, 8, 8);
      c.fillStyle = p.dim;
      c.fillText(it[1], cx + 12, y);
    });
  }

  var LAMP_LABEL = ['I1', 'I2', 'I3', 'I4', 'I5', 'I6', 'I7', 'I8'];
  var LAMP_HINT = ['terminates', 'acquired kept', 'no phantom', 'samples exact',
                   'recency', 'locked kept', 'complete flag', 'repeatable'];

  function drawLamps(g, v) {
    var c = g.ctx, p = g.pal, n = LAMP_LABEL.length;
    var w = (g.w * 0.62) / n;
    c.fillStyle = '#0a0e14';
    c.fillRect(0, LAMP_TOP - 6, g.w, g.h - LAMP_TOP + 6);
    for (var i = 0; i < n; i++) {
      var x = 16 + i * w, st = v.lamps[i] || 0;
      var col = st === 1 ? p.green : (st === -1 ? BAD : '#2a3543');
      c.fillStyle = col;
      c.beginPath();
      c.arc(x + 9, LAMP_TOP + 10, 7, 0, 6.284);
      c.fill();
      if (st === 1) {
        c.fillStyle = 'rgba(98,209,136,0.18)';
        c.beginPath(); c.arc(x + 9, LAMP_TOP + 10, 12, 0, 6.284); c.fill();
      }
      c.font = mono(11);
      c.fillStyle = st === 0 ? p.dim : col;
      c.fillText(LAMP_LABEL[i], x + 22, LAMP_TOP + 14);
      c.font = mono(9);
      c.fillStyle = p.dim;
      c.fillText(LAMP_HINT[i], x + 2, LAMP_TOP + 30);
    }
    c.textAlign = 'right';
    c.font = mono(10);
    c.fillStyle = p.dim;
    c.fillText(v.lampNote || 'the eight invariants, checked against what was injected',
               g.w - 16, LAMP_TOP + 14);
    c.textAlign = 'left';
  }

  return { drawStrip: drawStrip, drawLamps: drawLamps };
}());
