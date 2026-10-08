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
    }).observe(container);
    state.chart.subscribeCrosshairMove(updateLegend);
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
    if (!bars.length) { legend.innerHTML = ''; return; }

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

  window.chartApi = {
    init: function (theme) { state.theme = theme; document.body.style.background = theme.bg; ensureChart(); render(true); },
    setTheme: function (theme) { state.theme = theme; document.body.style.background = theme.bg; if (state.chart) render(false); },
    setBars: function (payload) { state.bars = payload.bars || []; state.meta = payload.meta || {}; render(true); },
    setOptions: function (opts) { state.options = Object.assign(state.options, opts); if (state.chart) render(false); },
    screenshot: function () { return state.chart && state.bars.length ? state.chart.takeScreenshot().toDataURL('image/png') : ''; },
    barCount: function () { return state.bars.length; },
    ready: true,
  };
})();
)js";

} // namespace

QString chartPageHtml()
{
    static const QString html = QStringLiteral(R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
  html, body { margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; background: #0a0f1c; }
  #chart { position: absolute; inset: 0; }
  #legend { position: absolute; left: 12px; top: 8px; z-index: 5; pointer-events: none;
            font: 12px Menlo, "SF Mono", monospace; color: #c7d2e3; display: flex; flex-wrap: wrap; gap: 0 14px; line-height: 20px; }
  #legend .sym { font-weight: 700; font-size: 14px; color: #f59e0b; }
  #legend .name, #legend .tf, #legend .asof { color: #8294ad; }
  #legend b { color: #f3f6fb; font-weight: 600; }
  #empty { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center;
           font: 13px -apple-system, sans-serif; color: #8294ad; }
</style></head>
<body>
<div id="chart"></div>
<div id="legend"></div>
<div id="empty">Select a ticker in the watchlist to load its chart.</div>
<script>%1</script>
<script>%2</script>
</body></html>)html").arg(QString::fromUtf8(kLightweightChartsJs), QString::fromUtf8(kControllerJs));
    return html;
}
