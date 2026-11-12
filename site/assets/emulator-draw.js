/*
 * emulator-draw.js — canvas drawing for site/emulator.html.
 *
 * Two pictures, both drawn from numbers the C module produced:
 *   drawTimeline  the op trace (every write, flush and durability mark) with
 *                 the cut line the visitor chose;
 *   drawDisk      the journal image, block by block, before and after recovery.
 *
 * No library: 2D canvas only, colours read from the site stylesheet so the
 * dark and light themes stay in sync.
 */
window.AmbreDraw = (function () {
  'use strict';

  var CSS_VAR = ['--bg-soft', '--line', '--ink', '--ink-dim', '--amber',
                 '--teal', '--red', '--green', '--bg-card'];

  function palette() {
    var style = getComputedStyle(document.documentElement);
    var out = {};
    CSS_VAR.forEach(function (name) {
      out[name.slice(2)] = style.getPropertyValue(name).trim() || '#888';
    });
    return out;
  }

  /* Size the backing store to the device pixel ratio, so lines stay sharp. */
  function prepare(canvas, cssHeight) {
    var dpr = window.devicePixelRatio || 1;
    var width = canvas.clientWidth || canvas.parentNode.clientWidth || 960;
    canvas.width = Math.max(1, Math.round(width * dpr));
    canvas.height = Math.max(1, Math.round(cssHeight * dpr));
    canvas.style.height = cssHeight + 'px';
    var ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, cssHeight);
    return { ctx: ctx, w: width, h: cssHeight, pal: palette() };
  }

  function opColour(op, pal) {
    if (op[0] === 2) return pal.amber;              /* flush */
    if (op[0] === 3) {                              /* durability mark */
      if (op[3] === 1) return pal.red;              /* EMERG written */
      if (op[3] === 2) return pal.green;            /* acquired */
      if (op[3] === 3) return pal.teal;             /* sealed */
      return pal['ink-dim'];
    }
    return pal['ink-dim'];                          /* write */
  }

  /* The trace as a strip: one column per op, marks drawn taller. */
  function drawTimeline(canvas, ops, cut) {
    var g = prepare(canvas, 74);
    var n = ops.length || 1;
    var colW = g.w / n;
    var i, op, x, h;

    g.ctx.fillStyle = g.pal['bg-soft'];
    g.ctx.fillRect(0, 0, g.w, g.h);
    for (i = 0; i < ops.length; i++) {
      op = ops[i];
      x = i * colW;
      h = op[0] === 1 ? 22 : (op[0] === 2 ? 34 : 46);
      g.ctx.fillStyle = opColour(op, g.pal);
      g.ctx.fillRect(x, g.h - 18 - h, Math.max(colW, 1), h);
    }
    /* The cut: where the 12 V rail dies. */
    x = cut * colW;
    g.ctx.fillStyle = g.pal.red;
    g.ctx.fillRect(x - 1, 0, 2, g.h);
    g.ctx.fillStyle = g.pal.ink;
    g.ctx.font = '11px ' + 'ui-monospace, monospace';
    g.ctx.fillText('power cut after op ' + cut, Math.min(x + 5, g.w - 150), 12);
    g.ctx.fillStyle = g.pal['ink-dim'];
    g.ctx.fillText('0', 2, g.h - 5);
    g.ctx.fillText(String(ops.length), g.w - 34, g.h - 5);
  }

  /* Run-length block map -> one code per block index. */
  function expandRuns(runs, total) {
    var codes = new Int8Array(total);
    runs.forEach(function (r) {
      var start = r[0] * 4096 + r[1];
      for (var k = 0; k < r[2]; k++) {
        if (start + k < total) codes[start + k] = r[3];
      }
    });
    return codes;
  }

  var CODE_COLOUR = { 1: 'ink-dim', 2: 'red', 3: 'amber', 4: 'green', 5: 'teal' };

  function usedRange(codes) {
    var first = -1, last = -1;
    for (var i = 0; i < codes.length; i++) {
      if (codes[i] !== 0) { if (first < 0) first = i; last = i; }
    }
    return { first: first, last: last };
  }

  /* The journal image: 128 blocks per row, one row per 128 blocks used. */
  function drawDisk(canvas, image, label) {
    var runs = (image && image.runs) || [];
    var total = (image && image.total) || 8192;
    var codes = expandRuns(runs, total);
    var range = usedRange(codes);
    var cols = 128, cell = 4, gap = 1;
    var count = range.first < 0 ? 0 : range.last - range.first + 1;
    var rows = Math.max(1, Math.ceil(count / cols));
    var g = prepare(canvas, Math.min(340, rows * (cell + gap) + 34));
    var i, code, x, y;

    g.ctx.fillStyle = g.pal['bg-soft'];
    g.ctx.fillRect(0, 0, g.w, g.h);
    g.ctx.fillStyle = g.pal['ink-dim'];
    g.ctx.font = '11px ui-monospace, monospace';
    g.ctx.fillText(label, 2, 12);
    for (i = 0; i < count; i++) {
      code = codes[range.first + i];
      x = 2 + (i % cols) * (cell + gap);
      y = 20 + Math.floor(i / cols) * (cell + gap);
      g.ctx.fillStyle = code === -1 ? g.pal.red
        : (g.pal[CODE_COLOUR[code] || 'line']);
      g.ctx.fillRect(x, y, cell, cell);
    }
    /* The two allocation-table copies, always at the very start. */
    drawTables(g, image);
  }

  function drawTables(g, image) {
    var y = g.h - 12;
    var cells = [['table A', image ? image.ta : 0], ['table B', image ? image.tb : 0]];
    g.ctx.font = '11px ui-monospace, monospace';
    var x = 2;
    cells.forEach(function (c) {
      g.ctx.fillStyle = c[1] === 1 ? g.pal.green : (c[1] === -1 ? g.pal.red : g.pal.line);
      g.ctx.fillRect(x, y - 8, 8, 8);
      g.ctx.fillStyle = g.pal['ink-dim'];
      g.ctx.fillText(c[0] + (image && c[0] === 'table A' ? ' gen ' + image.ga
        : (image ? ' gen ' + image.gb : '')), x + 12, y);
      x += 110;
    });
  }

  return { drawTimeline: drawTimeline, drawDisk: drawDisk, palette: palette };
}());
