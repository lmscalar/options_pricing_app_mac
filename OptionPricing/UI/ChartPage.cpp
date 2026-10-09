//
//  ChartPage.cpp
//  OptionPricing
//

#include "ChartPage.h"
#include "LightweightChartsJs.h"

namespace {

// The controller script. Bars arrive as {t (UTC seconds), o, h, l, c, v}; intraday bars are
// shifted into local time for display because the library renders UTC.
const char* const kControllerJs = R"js(
(function () {
  const state = {
    chart: null, main: null, volume: null, sma: null, ema: null,
    bars: [], meta: {}, theme: null,
    options: { type: 'candles', sma: { on: true, period: 20 }, ema: { on: true, period: 50 }, volume: true },
  };
  const container = document.getElementById('chart');
  const legend = document.getElementById('legend');
  const empty = document.getElementById('empty');

  function alpha(hex, a) {
    const n = parseInt(hex.replace('#', ''), 16);
    return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a + ')';
  }

  function toTime(sec, intraday) {
    if (intraday) {
      return sec - new Date(sec * 1000).getTimezoneOffset() * 60;   // show local wall-clock time
    }
    const d = new Date(sec * 1000);
    return { year: d.getUTCFullYear(), month: d.getUTCMonth() + 1, day: d.getUTCDate() };
  }

  function heikinAshi(bars) {
    const out = [];
    let prevOpen = null, prevClose = null;
    for (const b of bars) {
      const close = (b.o + b.h + b.l + b.c) / 4;
      const open = prevOpen === null ? (b.o + b.c) / 2 : (prevOpen + prevClose) / 2;
      out.push({ t: b.t, o: open, h: Math.max(b.h, open, close), l: Math.min(b.l, open, close), c: close, v: b.v });
      prevOpen = open; prevClose = close;
    }
    return out;
  }

  function sma(bars, period) {
    const out = []; let sum = 0;
    for (let i = 0; i < bars.length; i++) {
      sum += bars[i].c;
      if (i >= period) sum -= bars[i - period].c;
      if (i >= period - 1) out.push({ t: bars[i].t, value: sum / period });
    }
    return out;
  }

  function ema(bars, period) {
    const out = []; const k = 2 / (period + 1); let value = null;
    for (let i = 0; i < bars.length; i++) {
      value = value === null ? bars[i].c : bars[i].c * k + value * (1 - k);
      if (i >= period - 1) out.push({ t: bars[i].t, value: value });
    }
    return out;
  }

  function chartOptions(theme, intraday) {
    return {
      layout: { background: { type: 'solid', color: theme.bg }, textColor: theme.text, fontFamily: 'Menlo, SF Mono, monospace', fontSize: 11 },
      grid: { vertLines: { color: theme.grid }, horzLines: { color: theme.grid } },
      rightPriceScale: { borderColor: theme.border, scaleMargins: { top: 0.08, bottom: 0.25 } },
      timeScale: { borderColor: theme.border, timeVisible: !!intraday, secondsVisible: false, rightOffset: 4 },
      crosshair: { mode: 0, vertLine: { color: theme.crosshair, labelBackgroundColor: theme.accent }, horzLine: { color: theme.crosshair, labelBackgroundColor: theme.accent } },
      handleScroll: true, handleScale: true,
    };
  }

  function ensureChart() {
    if (state.chart) return;
    state.chart = LightweightCharts.createChart(container, chartOptions(state.theme, state.meta.intraday));
    new ResizeObserver(() => {
      state.chart.applyOptions({ width: container.clientWidth, height: container.clientHeight });
      redraw();
    }).observe(container);
    state.chart.subscribeCrosshairMove(p => { updateLegend(p); redraw(); });
    state.chart.timeScale().subscribeVisibleLogicalRangeChange(redraw);
  }

  function removeSeries() {
    for (const key of ['main', 'volume', 'sma', 'ema']) {
      if (state[key]) { state.chart.removeSeries(state[key]); state[key] = null; }
    }
  }

  function render(fit) {
    ensureChart();
    removeSeries();
    const theme = state.theme, opts = state.options, intraday = !!state.meta.intraday;
    state.chart.applyOptions(chartOptions(theme, intraday));
    const bars = opts.type === 'heikin' ? heikinAshi(state.bars) : state.bars;
    empty.style.display = bars.length ? 'none' : 'flex';
    if (!bars.length) { legend.innerHTML = ''; redraw(); return; }

    const data = bars.map(b => ({ time: toTime(b.t, intraday), open: b.o, high: b.h, low: b.l, close: b.c }));
    if (opts.type === 'bars') {
      state.main = state.chart.addBarSeries({ upColor: theme.up, downColor: theme.down, thinBars: false });
      state.main.setData(data);
    } else if (opts.type === 'line') {
      state.main = state.chart.addLineSeries({ color: theme.accent, lineWidth: 2 });
      state.main.setData(bars.map(b => ({ time: toTime(b.t, intraday), value: b.c })));
    } else {
      state.main = state.chart.addCandlestickSeries({
        upColor: theme.up, downColor: theme.down, borderUpColor: theme.up, borderDownColor: theme.down,
        wickUpColor: theme.up, wickDownColor: theme.down,
      });
      state.main.setData(data);
    }
    if (opts.volume) {
      state.volume = state.chart.addHistogramSeries({ priceFormat: { type: 'volume' }, priceScaleId: 'vol' });
      state.chart.priceScale('vol').applyOptions({ scaleMargins: { top: 0.82, bottom: 0 } });
      state.volume.setData(bars.map(b => ({ time: toTime(b.t, intraday), value: b.v, color: alpha(b.c >= b.o ? theme.up : theme.down, 0.45) })));
    }
    if (opts.sma && opts.sma.on && bars.length >= opts.sma.period) {
      state.sma = state.chart.addLineSeries({ color: theme.accent2, lineWidth: 1, priceLineVisible: false, lastValueVisible: false, crosshairMarkerVisible: false });
      state.sma.setData(sma(bars, opts.sma.period).map(p => ({ time: toTime(p.t, intraday), value: p.value })));
    }
    if (opts.ema && opts.ema.on && bars.length >= opts.ema.period) {
      state.ema = state.chart.addLineSeries({ color: theme.accent3, lineWidth: 1, priceLineVisible: false, lastValueVisible: false, crosshairMarkerVisible: false });
      state.ema.setData(ema(bars, opts.ema.period).map(p => ({ time: toTime(p.t, intraday), value: p.value })));
    }
    if (fit) state.chart.timeScale().fitContent();
    updateLegend(null);
    redraw();
  }

  function fmt(v, digits) { return (v === undefined || v === null || isNaN(v)) ? '–' : Number(v).toFixed(digits === undefined ? 2 : digits); }
  function fmtVolume(v) { return v >= 1e6 ? (v / 1e6).toFixed(2) + 'M' : v >= 1e3 ? (v / 1e3).toFixed(1) + 'k' : fmt(v, 0); }

  function updateLegend(param) {
    if (!state.bars.length) { legend.innerHTML = ''; return; }
    const theme = state.theme, opts = state.options;
    const source = opts.type === 'heikin' ? heikinAshi(state.bars) : state.bars;
    let bar = source[source.length - 1];
    if (param && param.time !== undefined && state.main) {
      const d = param.seriesData.get(state.main);
      if (d && d.open !== undefined) bar = { o: d.open, h: d.high, l: d.low, c: d.close, v: bar.v };
      else if (d && d.value !== undefined) bar = { o: d.value, h: d.value, l: d.value, c: d.value, v: bar.v };
      if (state.volume) { const vd = param.seriesData.get(state.volume); if (vd) bar.v = vd.value; }
    }
    const first = state.bars[0];
    const change = bar.c - (state.meta.previousClose || first.o);
    const pct = (state.meta.previousClose || first.o) ? change / (state.meta.previousClose || first.o) * 100 : 0;
    const color = change >= 0 ? theme.up : theme.down;
    const typeName = { candles: 'Candles', bars: 'Bars', heikin: 'Heikin-Ashi', line: 'Line' }[opts.type] || '';
    let html = '<span class="sym">' + (state.meta.symbol || '') + '</span>';
    if (state.meta.name) html += '<span class="name">' + state.meta.name + '</span>';
    html += '<span class="tf">' + (state.meta.timeframe || '') + ' · ' + typeName + '</span>';
    html += '<span>O <b>' + fmt(bar.o) + '</b></span><span>H <b>' + fmt(bar.h) + '</b></span><span>L <b>' + fmt(bar.l) + '</b></span>';
    html += '<span>C <b style="color:' + color + '">' + fmt(bar.c) + '</b></span>';
    html += '<span style="color:' + color + '">' + (change >= 0 ? '+' : '') + fmt(change) + ' (' + (change >= 0 ? '+' : '') + fmt(pct) + '%)</span>';
    html += '<span>Vol <b>' + fmtVolume(bar.v || 0) + '</b></span>';
    if (opts.sma && opts.sma.on) html += '<span style="color:' + theme.accent2 + '">SMA ' + opts.sma.period + '</span>';
    if (opts.ema && opts.ema.on) html += '<span style="color:' + theme.accent3 + '">EMA ' + opts.ema.period + '</span>';
    if (state.meta.asOf) html += '<span class="asof">' + state.meta.asOf + '</span>';
    legend.innerHTML = html;
  }

  // ---- Drawing layer -------------------------------------------------------------------
  // Trend lines and support/resistance zones live on a transparent canvas over the chart.
  // Points are stored as (bar time + fractional bar offset, price) so they survive
  // scrolling, zooming, timeframe changes and reloads; pixels are derived on every redraw
  // from the visible logical range and the main series' price scale.
  const draw = document.getElementById('draw');
  const dctx = draw.getContext('2d');
  const D = { tool: 'cursor', items: [], selected: null, pending: null, drag: null, nextId: 1 };

  function notify(kind, payload) { console.log('__' + kind + '__' + (payload === undefined ? '' : JSON.stringify(payload))); }
  function emitDrawings() { notify('drawings', { symbol: state.meta.symbol || '', items: D.items }); }

  function setTool(tool) {
    D.tool = tool; D.pending = null; D.drag = null;
    if (tool !== 'edit') D.selected = null;
    draw.style.pointerEvents = tool === 'cursor' ? 'none' : 'auto';
    draw.style.cursor = tool === 'edit' ? 'default' : 'crosshair';
    redraw();
    notify('tool', tool);
  }

  function paneRect() {
    const ts = state.chart.timeScale();
    return { w: ts.width(), h: container.clientHeight - ts.height() };
  }
  function logicalOfX(x) {
    const r = state.chart.timeScale().getVisibleLogicalRange(); const w = paneRect().w;
    return (r && w) ? r.from + x / w * (r.to - r.from) : null;
  }
  function xOfLogical(l) {
    const r = state.chart.timeScale().getVisibleLogicalRange(); const w = paneRect().w;
    return (r && w && r.to !== r.from) ? (l - r.from) / (r.to - r.from) * w : null;
  }
  function priceMap() {
    if (!state.main || !state.bars.length) return null;
    const p0 = state.bars[state.bars.length - 1].c;
    const y0 = state.main.priceToCoordinate(p0), y1 = state.main.priceToCoordinate(p0 * 1.02);
    if (y0 === null || y1 === null || y0 === y1) return null;
    return { p0: p0, y0: y0, k: (y1 - y0) / (p0 * 0.02) };
  }
  function yOfPrice(p) { const m = priceMap(); return m ? m.y0 + m.k * (p - m.p0) : null; }
  function priceOfY(y) { const m = priceMap(); return m ? m.p0 + (y - m.y0) / m.k : null; }

  function anchorFromLogical(l) {
    const n = state.bars.length; if (!n) return null;
    const i = Math.min(n - 1, Math.max(0, Math.round(l)));
    return { t: state.bars[i].t, extra: l - i };
  }
  function logicalFromAnchor(a) {
    const n = state.bars.length; if (!n || !a) return null;
    let lo = 0, hi = n - 1;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (state.bars[mid].t < a.t) lo = mid + 1; else hi = mid; }
    if (lo > 0 && Math.abs(state.bars[lo - 1].t - a.t) <= Math.abs(state.bars[lo].t - a.t)) lo -= 1;
    return lo + (a.extra || 0);
  }
  function trendPoints(d) {
    const la = logicalFromAnchor(d.a), lb = logicalFromAnchor(d.b);
    if (la === null || lb === null) return null;
    const x1 = xOfLogical(la), x2 = xOfLogical(lb), y1 = yOfPrice(d.pa), y2 = yOfPrice(d.pb);
    if ([x1, x2, y1, y2].some(v => v === null || isNaN(v))) return null;
    return { x1: x1, y1: y1, x2: x2, y2: y2 };
  }
  function rayEnd(p, w) {
    const s = p.x2 >= p.x1 ? { x: p.x1, y: p.y1 } : { x: p.x2, y: p.y2 };
    const e = p.x2 >= p.x1 ? { x: p.x2, y: p.y2 } : { x: p.x1, y: p.y1 };
    if (e.x === s.x) return null;
    return { sx: e.x, sy: e.y, ex: w, ey: e.y + (e.y - s.y) / (e.x - s.x) * (w - e.x) };
  }

  function line(x1, y1, x2, y2) { dctx.beginPath(); dctx.moveTo(x1, y1); dctx.lineTo(x2, y2); dctx.stroke(); }
  function handle(x, y, color) {
    dctx.beginPath(); dctx.arc(x, y, 5, 0, Math.PI * 2);
    dctx.fillStyle = state.theme.bg; dctx.fill();
    dctx.lineWidth = 2; dctx.strokeStyle = color; dctx.setLineDash([]); dctx.stroke();
  }
  function drawZone(d, pane, sel) {
    const hi = Math.max(d.lo, d.hi), lo = Math.min(d.lo, d.hi);
    const yT = yOfPrice(hi), yB = yOfPrice(lo); if (yT === null || yB === null) return;
    const color = d.kind === 'support' ? state.theme.up : state.theme.down;
    dctx.fillStyle = alpha(color, sel ? 0.28 : 0.16);
    dctx.fillRect(0, yT, pane.w, Math.max(1, yB - yT));
    dctx.strokeStyle = alpha(color, 0.9); dctx.lineWidth = sel ? 2 : 1; dctx.setLineDash([]);
    line(0, yT, pane.w, yT); line(0, yB, pane.w, yB);
    const label = (d.kind === 'support' ? 'Support ' : 'Resistance ') + fmt(lo) + ' – ' + fmt(hi);
    dctx.font = '600 11px Menlo, "SF Mono", monospace'; dctx.textAlign = 'right'; dctx.textBaseline = 'bottom';
    const tw = dctx.measureText(label).width;
    const ly = yT >= 18 ? yT - 2 : yB + 15;
    dctx.fillStyle = alpha(state.theme.bg, 0.85); dctx.fillRect(pane.w - tw - 16, ly - 14, tw + 10, 15);
    dctx.fillStyle = color; dctx.fillText(label, pane.w - 11, ly);
    if (sel) { handle(pane.w / 2, yT, color); handle(pane.w / 2, yB, color); }
  }
  function drawTrend(d, pane, sel, pending) {
    const p = trendPoints(d); if (!p) return;
    const color = state.theme.accent2;
    dctx.strokeStyle = color; dctx.lineWidth = sel ? 3 : 2; dctx.setLineDash([]);
    line(p.x1, p.y1, p.x2, p.y2);
    if (d.extend !== false) {
      const r = rayEnd(p, pane.w);
      if (r) { dctx.setLineDash([6, 5]); dctx.lineWidth = sel ? 2 : 1.5; line(r.sx, r.sy, r.ex, r.ey); dctx.setLineDash([]); }
    }
    if (sel || pending) {
      handle(p.x1, p.y1, color); handle(p.x2, p.y2, color);
      dctx.font = '11px Menlo, "SF Mono", monospace'; dctx.fillStyle = color; dctx.textAlign = 'left'; dctx.textBaseline = 'bottom';
      dctx.fillText(fmt(d.pa), p.x1 + 8, p.y1 - 5); dctx.fillText(fmt(d.pb), p.x2 + 8, p.y2 - 5);
    }
  }
  function redraw() {
    if (!state.chart) return;
    const w = container.clientWidth, h = container.clientHeight, dpr = window.devicePixelRatio || 1;
    if (draw.width !== Math.round(w * dpr) || draw.height !== Math.round(h * dpr)) { draw.width = Math.round(w * dpr); draw.height = Math.round(h * dpr); }
    draw.style.width = w + 'px'; draw.style.height = h + 'px';
    dctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    dctx.clearRect(0, 0, w, h);
    if (!state.bars.length || !state.main) return;
    const pane = paneRect(); if (!pane.w || !pane.h) return;
    dctx.save(); dctx.beginPath(); dctx.rect(0, 0, pane.w, pane.h); dctx.clip();
    const all = D.pending ? D.items.concat([D.pending]) : D.items;
    for (const d of all) {
      const sel = D.selected === d.id;
      if (d.type === 'zone') drawZone(d, pane, sel); else drawTrend(d, pane, sel, d === D.pending);
    }
    dctx.restore();
  }

  function distToSegment(px, py, x1, y1, x2, y2) {
    const dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
    let t = len2 ? ((px - x1) * dx + (py - y1) * dy) / len2 : 0; t = Math.max(0, Math.min(1, t));
    return Math.hypot(px - (x1 + t * dx), py - (y1 + t * dy));
  }
  function hitTest(x, y, pane) {
    for (let i = D.items.length - 1; i >= 0; i--) {       // handles and edges first
      const d = D.items[i];
      if (d.type === 'trend') {
        const p = trendPoints(d); if (!p) continue;
        if (Math.hypot(x - p.x1, y - p.y1) <= 8) return { d: d, part: 'a' };
        if (Math.hypot(x - p.x2, y - p.y2) <= 8) return { d: d, part: 'b' };
      } else {
        const yT = yOfPrice(Math.max(d.lo, d.hi)), yB = yOfPrice(Math.min(d.lo, d.hi));
        if (Math.abs(y - yT) <= 6) return { d: d, part: 'hi' };
        if (Math.abs(y - yB) <= 6) return { d: d, part: 'lo' };
      }
    }
    for (let i = D.items.length - 1; i >= 0; i--) {       // then bodies
      const d = D.items[i];
      if (d.type === 'trend') {
        const p = trendPoints(d); if (!p) continue;
        let hit = distToSegment(x, y, p.x1, p.y1, p.x2, p.y2) <= 6;
        if (!hit && d.extend !== false) { const r = rayEnd(p, pane.w); if (r) hit = distToSegment(x, y, r.sx, r.sy, r.ex, r.ey) <= 6; }
        if (hit) return { d: d, part: 'body' };
      } else {
        const yT = yOfPrice(Math.max(d.lo, d.hi)), yB = yOfPrice(Math.min(d.lo, d.hi));
        if (y >= yT && y <= yB) return { d: d, part: 'body' };
      }
    }
    return null;
  }
  function normalize(d) { if (d.type === 'zone' && d.lo > d.hi) { const t = d.lo; d.lo = d.hi; d.hi = t; } }
  function finishPending() {
    const d = D.pending; D.pending = null;
    delete d.startX; delete d.startY; delete d.await;
    normalize(d);
    D.items.push(d);
    emitDrawings();
    setTool('cursor');
  }
  function deleteSelected() {
    if (D.selected === null) return;
    D.items = D.items.filter(d => d.id !== D.selected); D.selected = null;
    emitDrawings(); redraw();
  }
  function pos(e) { const r = draw.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top }; }

  draw.addEventListener('mousedown', e => {
    if (!state.bars.length || !state.main || e.button !== 0) return;
    const p = pos(e), pane = paneRect(); if (p.x > pane.w || p.y > pane.h) return;
    const l = logicalOfX(p.x), price = priceOfY(p.y); if (l === null || price === null) return;
    e.preventDefault();
    if (D.tool === 'trend') {
      if (D.pending && D.pending.await) { D.pending.b = anchorFromLogical(l); D.pending.pb = price; finishPending(); return; }
      D.pending = { id: D.nextId++, type: 'trend', a: anchorFromLogical(l), pa: price, b: anchorFromLogical(l), pb: price, extend: true, startX: p.x, startY: p.y };
    } else if (D.tool === 'support' || D.tool === 'resistance') {
      D.pending = { id: D.nextId++, type: 'zone', kind: D.tool, lo: price, hi: price, startY: p.y };
    } else if (D.tool === 'edit') {
      const hit = hitTest(p.x, p.y, pane);
      if (hit) { D.selected = hit.d.id; D.drag = { d: hit.d, part: hit.part, l0: l, price0: price, orig: JSON.parse(JSON.stringify(hit.d)) }; }
      else D.selected = null;
    }
    redraw();
  });
  draw.addEventListener('mousemove', e => {
    if (!state.bars.length || !state.main) return;
    const p = pos(e); const l = logicalOfX(p.x), price = priceOfY(p.y); if (l === null || price === null) return;
    if (D.pending) {
      if (D.pending.type === 'trend') { D.pending.b = anchorFromLogical(l); D.pending.pb = price; } else { D.pending.hi = price; }
      redraw();
    } else if (D.drag) {
      const g = D.drag, d = g.d, o = g.orig;
      if (d.type === 'trend') {
        if (g.part === 'a') { d.a = anchorFromLogical(l); d.pa = price; }
        else if (g.part === 'b') { d.b = anchorFromLogical(l); d.pb = price; }
        else {
          const dl = l - g.l0, dp = price - g.price0;
          d.a = anchorFromLogical(logicalFromAnchor(o.a) + dl); d.b = anchorFromLogical(logicalFromAnchor(o.b) + dl);
          d.pa = o.pa + dp; d.pb = o.pb + dp;
        }
      } else {
        const dp = price - g.price0;
        if (g.part === 'hi') d.hi = price; else if (g.part === 'lo') d.lo = price; else { d.lo = o.lo + dp; d.hi = o.hi + dp; }
      }
      redraw();
    } else if (D.tool === 'edit') {
      const hit = hitTest(p.x, p.y, paneRect());
      draw.style.cursor = hit ? (hit.part === 'body' ? 'move' : (hit.d.type === 'zone' ? 'ns-resize' : 'pointer')) : 'default';
    }
  });
  window.addEventListener('mouseup', e => {
    if (D.pending) {
      const p = pos(e);
      if (D.pending.type === 'trend') {
        if (Math.hypot(p.x - D.pending.startX, p.y - D.pending.startY) < 4) { D.pending.await = true; return; }   // click, move, click
        finishPending();
      } else {
        if (Math.abs(p.y - D.pending.startY) < 3) { const mid = D.pending.lo; D.pending.lo = mid * 0.997; D.pending.hi = mid * 1.003; }
        finishPending();
      }
    } else if (D.drag) {
      normalize(D.drag.d); D.drag = null; emitDrawings(); redraw();
    }
  });
  window.addEventListener('keydown', e => {
    if (e.key === 'Escape') { if (D.pending) { D.pending = null; redraw(); } else setTool('cursor'); }
    else if ((e.key === 'Delete' || e.key === 'Backspace') && D.tool === 'edit' && D.selected !== null) { e.preventDefault(); deleteSelected(); }
  });
  setInterval(redraw, 400);   // catches price-scale drags, which fire no range event

  window.chartApi = {
    init: function (theme) { state.theme = theme; document.body.style.background = theme.bg; ensureChart(); render(true); },
    setTheme: function (theme) { state.theme = theme; document.body.style.background = theme.bg; if (state.chart) render(false); },
    setBars: function (payload) {
      const meta = payload.meta || {};
      if ((meta.symbol || '') !== (state.meta.symbol || '')) { D.items = []; D.selected = null; D.pending = null; }
      state.bars = payload.bars || []; state.meta = meta; render(true);
    },
    setOptions: function (opts) { state.options = Object.assign(state.options, opts); if (state.chart) render(false); },
    screenshot: function () {
      if (!state.chart || !state.bars.length) return '';
      const shot = state.chart.takeScreenshot();
      if (!D.items.length) return shot.toDataURL('image/png');
      const out = document.createElement('canvas'); out.width = shot.width; out.height = shot.height;
      const ctx = out.getContext('2d'); ctx.drawImage(shot, 0, 0);
      ctx.drawImage(draw, 0, 0, draw.width, draw.height, 0, 0, shot.width, shot.height);
      return out.toDataURL('image/png');
    },
    barCount: function () { return state.bars.length; },
    // Drawing tools
    setTool: setTool,
    setDrawings: function (items) {
      D.items = Array.isArray(items) ? items : []; D.selected = null; D.pending = null;
      D.nextId = D.items.reduce((m, d) => Math.max(m, d.id || 0), 0) + 1;
      redraw();
    },
    drawings: function () { return JSON.stringify(D.items); },
    undoDrawing: function () { if (D.items.length) { D.items.pop(); D.selected = null; emitDrawings(); redraw(); } },
    clearDrawings: function () { if (D.items.length) { D.items = []; D.selected = null; emitDrawings(); redraw(); } },
    deleteSelected: deleteSelected,
    // Test hook: draws a trend line and two zones through the real mouse handlers.
    simulateDrawings: function () {
      if (!state.bars.length || !state.main) return 0;
      const pane = paneRect(), n = state.bars.length;
      const ev = (type, x, y) => {
        const r = draw.getBoundingClientRect();
        (type === 'mouseup' ? window : draw).dispatchEvent(new MouseEvent(type, { clientX: r.left + x, clientY: r.top + y, bubbles: true, button: 0 }));
      };
      const i1 = Math.floor(n * 0.15), i2 = Math.floor(n * 0.75);
      setTool('trend');
      ev('mousedown', xOfLogical(i1), yOfPrice(state.bars[i1].l)); ev('mousemove', xOfLogical(i2), yOfPrice(state.bars[i2].l)); ev('mouseup', xOfLogical(i2), yOfPrice(state.bars[i2].l));
      const recent = state.bars.slice(Math.floor(n * 0.7));
      const hi = Math.max(...recent.map(b => b.h)), lo = Math.min(...recent.map(b => b.l));
      setTool('resistance');
      ev('mousedown', pane.w / 2, yOfPrice(hi)); ev('mousemove', pane.w / 2, yOfPrice(hi * 0.985)); ev('mouseup', pane.w / 2, yOfPrice(hi * 0.985));
      setTool('support');
      ev('mousedown', pane.w / 2, yOfPrice(lo)); ev('mousemove', pane.w / 2, yOfPrice(lo * 1.015)); ev('mouseup', pane.w / 2, yOfPrice(lo * 1.015));
      return D.items.length;
    },
    ready: true,
  };
})();
)js";

} // namespace

ChartWebPage::ChartWebPage(QObject* parent)
    : QWebEnginePage(QWebEngineProfile::defaultProfile(), parent)
{
}

void ChartWebPage::javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString& message, int lineNumber, const QString& sourceID)
{
    // The page reports events as console lines of the form "__kind__payload".
    if (message.startsWith("__")) {
        const qsizetype end = message.indexOf("__", 2);
        if (end > 2 && onMessage) {
            onMessage(message.mid(2, end - 2), message.mid(end + 2));
            return;
        }
    }
    QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber, sourceID);
}

QString chartPageHtml()
{
    static const QString html = QStringLiteral(R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
  html, body { margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; background: #0a0f1c; }
  #chart { position: absolute; inset: 0; }
  #draw { position: absolute; left: 0; top: 0; z-index: 4; pointer-events: none; }
  #legend { position: absolute; left: 12px; top: 8px; z-index: 5; pointer-events: none;
            font: 12px Menlo, "SF Mono", monospace; color: #c7d2e3; display: flex; flex-wrap: wrap; gap: 0 14px; line-height: 20px; }
  #legend .sym { font-weight: 700; font-size: 14px; color: #f59e0b; }
  #legend .name, #legend .tf, #legend .asof { color: #8294ad; }
  #legend b { color: #f3f6fb; font-weight: 600; }
  #empty { position: absolute; inset: 0; z-index: 6; display: flex; align-items: center; justify-content: center;
           font: 13px -apple-system, sans-serif; color: #8294ad; }
</style></head>
<body>
<div id="chart"></div>
<canvas id="draw"></canvas>
<div id="legend"></div>
<div id="empty">Select a ticker in the watchlist to load its chart.</div>
<script>%1</script>
<script>%2</script>
</body></html>)html").arg(QString::fromUtf8(kLightweightChartsJs), QString::fromUtf8(kControllerJs));
    return html;
}
