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
    chart: null, main: null, volume: null,
    overlays: [],            // indicators drawn on the price scale (entries of state.indicators)
    // Indicator results computed by the host with TA-Lib: [{id, func, label, shortLabel,
    // placement:'overlay'|'pane'|'markers', precision, levels, outputs:[{name, short, style,
    // color, zero, negative, upper, lower, values:[aligned with bars, null = none]}]}].
    indicators: [],
    // Oscillators (MACD today) live in framed panes below the main chart, each a second
    // chart whose time scale and crosshair are linked to the main one: [{id, spec, el,
    // chart, legendEl, series:{...}, data, syncing}].
    panes: [],
    bars: [], meta: {}, theme: null, live: null, liveLine: null, liveLineSeries: null, syncingCrosshair: false,
    // Implied move cone from the option market (host-computed, see QuotesTab::pushEventCone):
    // {enabled, symbol, points:[{t, up1, dn1, up2, dn2}], event:{t, label, up, down, up1, dn1}, caption}
    eventCone: null, coneSeries: [], coneFitFor: null, coneNeedsFit: false,
    // Comparison overlays (host-fetched, see QuotesTab::pushCompare): [{symbol, color,
    // points:[{t, c}]}]. With any present the right scale switches to percentage mode, so
    // every series reads as % change from the first visible bar.
    compare: [], compareSeries: [],
    // True until the user pans or zooms: re-renders then keep everything in view (the library's
    // fitContent is applied lazily, so a stored range read right after it can be stale).
    autoFit: true,
    // indicators: [{type:'sma'|'ema', period, color}, {type:'macd', fast, slow, signal}] (at
    // most three SMAs, three EMAs and one MACD; the host enforces the limits).
    options: { type: 'candles', volume: true, priceLine: true, paneHeight: 0.24 },
  };
  const container = document.getElementById('chart');
  const panesEl = document.getElementById('panes');
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

  function paneIndicators() { return state.indicators.filter(i => i && i.placement === 'pane'); }
  function timeOf(i) { return toTime(state.bars[i].t, !!state.meta.intraday); }
  function indexOfTime(t) {
    const intraday = !!state.meta.intraday;
    for (let i = state.bars.length - 1; i >= 0; i--) { if (sameTime(toTime(state.bars[i].t, intraday), t)) return i; }
    return -1;
  }
  function valueAt(values, idx) {
    if (!values) return undefined;
    if (idx >= 0) return values[idx];
    for (let i = values.length - 1; i >= 0; i--) { if (values[i] !== null && values[i] !== undefined) return values[i]; }
    return undefined;
  }

  // Main chart layout: the volume strip, when on, occupies the bottom of the price pane
  // (as on professional platforms); oscillators get their own panes below (see panes).
  function layout() {
    const vol = !!state.options.volume;
    return vol ? { priceBottom: 0.25, vol: { top: 0.82, bottom: 0 } } : { priceBottom: 0.08, vol: null };
  }
  function wantedPanes() { return paneIndicators().map(i => 'ind' + i.id); }
  const PANE_GAP = 8;            // px between the main chart and the first pane (the drag handle), and between panes
  const AXIS_WIDTH = 78;         // fixed right-axis width so the main chart and the panes line up
  const PANE_DEFAULT = 0.24, PANE_EXPANDED = 0.5, PANE_MIN = 0.1, PANE_MAX = 0.65;
  // Pane height as a fraction of the chart area; the user drags the handle or toggles expand,
  // and the host remembers the value (options.paneHeight).
  function paneFraction() {
    const f = Number(state.options.paneHeight);
    return isNaN(f) || f <= 0 ? PANE_DEFAULT : Math.max(PANE_MIN, Math.min(PANE_MAX, f));
  }
  function paneHeight() {
    // Per-pane height from the preferred fraction, but all panes together never take more
    // than PANE_MAX of the chart, so the price chart keeps room however many panes are open.
    const total = document.body.clientHeight;
    const n = Math.max(1, wantedPanes().length);
    const perPaneCap = Math.floor((total * PANE_MAX - n * PANE_GAP) / n);
    return Math.max(60, Math.min(Math.round(total * paneFraction()), perPaneCap));
  }
  // Sizes the main chart and the pane strip; the charts follow via their ResizeObservers.
  function applyLayout() {
    const n = wantedPanes().length;
    const h = paneHeight();
    const stripHeight = n ? n * (h + PANE_GAP) : 0;
    container.style.bottom = stripHeight + 'px';
    panesEl.style.height = stripHeight + 'px';
    panesEl.style.display = n ? 'block' : 'none';
    paneHandle.style.display = n ? 'block' : 'none';
    paneHandle.style.bottom = (stripHeight - PANE_GAP) + 'px';
    for (const pane of state.panes) pane.el.style.height = h + 'px';
    // Size the charts right away rather than waiting for the ResizeObservers, which only
    // run with the next frame (and lag when the view is off screen, e.g. during an export).
    if (state.chart) state.chart.applyOptions({ width: container.clientWidth, height: container.clientHeight });
    for (const pane of state.panes) {
      const chartEl = pane.el.firstChild;
      if (chartEl) pane.chart.applyOptions({ width: chartEl.clientWidth, height: chartEl.clientHeight });
    }
    updateExpandButtons();
    // The library applies the new size with its next frame; redraw the overlay then too.
    if (state.chart) { redraw(); requestAnimationFrame(redraw); setTimeout(redraw, 60); }
  }
  function setPaneFraction(f, persist) {
    state.options.paneHeight = Math.max(PANE_MIN, Math.min(PANE_MAX, f));
    applyLayout();
    if (persist) notify('options', { paneHeight: state.options.paneHeight });
  }
  function paneExpanded() { return paneFraction() >= (PANE_DEFAULT + PANE_EXPANDED) / 2; }
  function toggleExpanded() { setPaneFraction(paneExpanded() ? PANE_DEFAULT : PANE_EXPANDED, true); }
  function updateExpandButtons() {
    const expanded = paneExpanded();
    for (const pane of state.panes) {
      if (!pane.expandBtn) continue;
      pane.expandBtn.textContent = expanded ? '⤓' : '⤢';
      pane.expandBtn.title = expanded ? 'Restore the pane height' : 'Expand the pane';
    }
  }

  // "Auto" button on the price axis: shown only while the axis has been dragged to a manual
  // scale; clicking it restores autoscale (so does double-clicking the axis).
  const autoBtn = document.getElementById('auto-scale');
  autoBtn.addEventListener('click', () => {
    if (!state.chart) return;
    state.chart.priceScale('right').applyOptions({ autoScale: true });
    updateAutoButton();
    redraw();
  });
  function priceScaleIsManual() {
    if (!state.chart || !state.main) return false;
    try { return state.chart.priceScale('right').options().autoScale === false; } catch (e) { return false; }
  }
  function updateAutoButton() {
    const manual = priceScaleIsManual();
    autoBtn.style.display = manual ? 'block' : 'none';
    if (!manual) return;
    const tsHeight = wantedPanes().length ? 0 : state.chart.timeScale().height();
    autoBtn.style.bottom = (parseFloat(container.style.bottom) || 0) + tsHeight + 6 + 'px';
  }
  // Dragging an axis fires no chart event the overlay could follow; redraw on mouse moves
  // inside the chart instead (cheap: the overlay is only a few shapes).
  container.addEventListener('wheel', () => { state.autoFit = false; }, { passive: true });
  container.addEventListener('mousedown', () => { state.autoFit = false; });
  container.addEventListener('mousemove', () => { if (state.chart) { redraw(); updateAutoButton(); } });
  container.addEventListener('mouseup', () => { if (state.chart) { redraw(); updateAutoButton(); } });
  container.addEventListener('dblclick', () => { if (state.chart) setTimeout(() => { redraw(); updateAutoButton(); }, 0); });

  // Drag handle between the price chart and the indicator panes (double-click toggles expand).
  const paneHandle = document.getElementById('pane-handle');
  (function () {
    let dragging = null;
    paneHandle.addEventListener('mousedown', e => {
      if (e.button !== 0) return;
      dragging = { startY: e.clientY, startH: paneHeight() };
      paneHandle.classList.add('active');
      e.preventDefault();
    });
    window.addEventListener('mousemove', e => {
      if (!dragging) return;
      const n = Math.max(1, wantedPanes().length);
      const h = dragging.startH + (dragging.startY - e.clientY) / n;
      setPaneFraction(h / Math.max(1, document.body.clientHeight), false);
    });
    window.addEventListener('mouseup', () => {
      if (!dragging) return;
      dragging = null;
      paneHandle.classList.remove('active');
      notify('options', { paneHeight: paneFraction() });
    });
    paneHandle.addEventListener('dblclick', toggleExpanded);
  })();

  // Fonts follow the host's text-size setting (theme.fontScale) and tighten in compact mode.
  function fontScale() { return ((state.theme && state.theme.fontScale) || 1) * (state.options.compact ? 0.9 : 1); }
  function axisFontSize() { return Math.max(8, Math.round(11 * fontScale())); }
  function applyFonts() {
    const s = fontScale();
    const root = document.documentElement.style;
    root.setProperty('--legend-size', Math.max(9, Math.round(12 * s)) + 'px');
    root.setProperty('--legend-line', Math.max(14, Math.round(20 * s)) + 'px');
    root.setProperty('--sym-size', Math.max(10, Math.round(14 * s)) + 'px');
    root.setProperty('--pane-legend-size', Math.max(9, Math.round(11 * s)) + 'px');
  }
  function chartOptions(theme, intraday) {
    return {
      layout: { background: { type: 'solid', color: theme.bg }, textColor: theme.text, fontFamily: 'Menlo, SF Mono, monospace', fontSize: axisFontSize() },
      grid: { vertLines: { color: theme.grid }, horzLines: { color: theme.grid } },
      rightPriceScale: { borderColor: theme.border, minimumWidth: AXIS_WIDTH, scaleMargins: { top: 0.08, bottom: layout().priceBottom } },
      // With panes below, only the bottom pane shows the time axis.
      timeScale: { borderColor: theme.border, visible: wantedPanes().length === 0, timeVisible: !!intraday, secondsVisible: false, rightOffset: 4 },
      crosshair: { mode: 0, vertLine: { color: theme.crosshair, labelBackgroundColor: theme.accent }, horzLine: { color: theme.crosshair, labelBackgroundColor: theme.accent } },
      handleScroll: true,
      // Both axes can be dragged to stretch or compress the scale. A dragged price axis
      // switches autoscale off; it comes back on every new load (render(fit)), on
      // double-click of the axis, with the Auto button that appears on the axis, and with Reset.
      handleScale: { mouseWheel: true, pinch: true, axisPressedMouseMove: { time: true, price: true }, axisDoubleClickReset: { time: true, price: true } },
    };
  }

  function ensureChart() {
    if (state.chart) return;
    applyLayout();
    state.chart = LightweightCharts.createChart(container, chartOptions(state.theme, state.meta.intraday));
    new ResizeObserver(() => {
      state.chart.applyOptions({ width: container.clientWidth, height: container.clientHeight });
      redraw();
    }).observe(container);
    new ResizeObserver(applyLayout).observe(document.body);
    state.chart.subscribeCrosshairMove(p => {
      updateLegend(p); redraw();
      // setCrosshairPosition on a pane re-enters through its own subscriber; the flag stops the ping-pong.
      if (state.syncingCrosshair) return;
      state.syncingCrosshair = true;
      try { syncPaneCrosshair(p); } finally { state.syncingCrosshair = false; }
    });
    state.chart.timeScale().subscribeVisibleLogicalRangeChange(r => { redraw(); syncPaneRange(r); });
  }

  // ---- Oscillator panes -----------------------------------------------------------------
  function paneOptions(theme, intraday) {
    const o = chartOptions(theme, intraday);
    o.rightPriceScale = { borderColor: theme.border, minimumWidth: AXIS_WIDTH, scaleMargins: { top: 0.12, bottom: 0.08 } };
    o.timeScale = { borderColor: theme.border, visible: true, timeVisible: !!intraday, secondsVisible: false, rightOffset: 4 };
    return o;
  }
  function createPane(id, title) {
    const theme = state.theme;
    const el = document.createElement('div');
    el.className = 'pane';
    el.style.height = paneHeight() + 'px';
    el.style.marginTop = PANE_GAP + 'px';
    el.style.borderColor = theme.border;
    el.style.background = theme.bg;
    const chartEl = document.createElement('div');
    chartEl.className = 'pane-chart';
    const legendEl = document.createElement('div');
    legendEl.className = 'pane-legend';
    const expandBtn = document.createElement('div');
    expandBtn.className = 'pane-btn';
    expandBtn.addEventListener('click', toggleExpanded);
    el.appendChild(chartEl); el.appendChild(legendEl); el.appendChild(expandBtn);
    panesEl.appendChild(el);
    const chart = LightweightCharts.createChart(chartEl, paneOptions(theme, !!state.meta.intraday));
    const pane = { id: id, title: title, el: el, chart: chart, legendEl: legendEl, expandBtn: expandBtn, series: {}, data: [], target: null, settled: false };
    updateExpandButtons();
    new ResizeObserver(() => chart.applyOptions({ width: chartEl.clientWidth, height: chartEl.clientHeight })).observe(chartEl);
    // The main chart leads: its range is pushed to the pane (syncPaneRange). Range events
    // arrive asynchronously, so a pane event that merely echoes the pushed range is ignored,
    // and nothing is pushed back until the pane has caught up once (its own default zoom
    // must never override the main chart's). After that, panning or zooming inside the pane
    // moves the main chart too.
    chart.timeScale().subscribeVisibleLogicalRangeChange(r => {
      if (!r || !pane.target) return;
      const echo = Math.abs(r.from - pane.target.from) < 1e-6 && Math.abs(r.to - pane.target.to) < 1e-6;
      if (echo) { pane.settled = true; return; }
      if (!pane.settled) return;
      state.chart.timeScale().setVisibleLogicalRange(r);
    });
    chart.subscribeCrosshairMove(p => {
      if (!state.main || !state.bars.length || state.syncingCrosshair) return;
      state.syncingCrosshair = true;
      try { paneHover(p); } finally { state.syncingCrosshair = false; }
    });
    function paneHover(p) {
      if (p && p.time !== undefined) {
        const intraday = !!state.meta.intraday;
        const bar = state.bars.find(b => sameTime(toTime(b.t, intraday), p.time));
        if (bar) { state.chart.setCrosshairPosition(bar.c, p.time, state.main); updateLegend({ time: p.time, seriesData: new Map(), fromPane: bar }); }
      } else {
        state.chart.clearCrosshairPosition();
        updateLegend(null);
      }
      updatePaneLegends(p && p.time !== undefined ? p.time : null);
    }
    state.panes.push(pane);
    return pane;
  }
  function removePanes() {
    for (const pane of state.panes) { try { pane.chart.remove(); } catch (e) {} if (pane.el.parentNode) pane.el.parentNode.removeChild(pane.el); }
    state.panes = [];
  }
  function sameTime(a, b) {
    if (a === undefined || b === undefined || a === null || b === null) return false;
    return typeof a === 'object' ? (a.year === b.year && a.month === b.month && a.day === b.day) : a === b;
  }
  function syncPaneRange(r) {
    if (!r) return;
    for (const pane of state.panes) {
      pane.target = { from: r.from, to: r.to };
      pane.chart.timeScale().setVisibleLogicalRange(r);
    }
  }
  function syncPaneCrosshair(p) {
    const hovering = p && p.time !== undefined;
    const idx = hovering ? indexOfTime(p.time) : -1;
    for (const pane of state.panes) {
      const ind = pane.indicator;
      const anchor = ind && ind.series ? ind.series[0] : null;
      if (!anchor) continue;
      const value = hovering && ind.outputs.length ? valueAt(ind.outputs[0].values, idx) : null;
      if (hovering && value !== null && value !== undefined) pane.chart.setCrosshairPosition(value, p.time, anchor);
      else pane.chart.clearCrosshairPosition();
    }
    updatePaneLegends(hovering ? p.time : null);
  }
  function updatePaneLegends(time) {
    const theme = state.theme;
    const idx = time === null ? -1 : indexOfTime(time);
    for (const pane of state.panes) {
      const ind = pane.indicator;
      if (!ind) continue;
      const titleColor = ind.outputs.length ? ind.outputs[0].color : theme.text;
      let html = '<span class="title" style="color:' + titleColor + '">' + ind.label + '</span>';
      for (const o of ind.outputs) {
        const v = valueAt(o.values, idx);
        const col = o.style === 'hist' ? ((v === undefined || v === null) ? theme.text : (v >= 0 ? theme.up : theme.down)) : o.color;
        html += '<span>' + (ind.outputs.length > 1 ? o.short + ' ' : '') + '<b style="color:' + col + '">' + fmt(v, ind.precision === undefined ? 3 : ind.precision) + '</b></span>';
      }
      pane.legendEl.innerHTML = html;
    }
  }

  function removeSeries() {
    for (const key of ['main', 'volume']) {
      if (state[key]) { state.chart.removeSeries(state[key]); state[key] = null; }
    }
    for (const ind of state.overlays) { for (const srs of (ind.series || [])) { try { state.chart.removeSeries(srs); } catch (e) {} } ind.series = []; }
    state.overlays = [];
    for (const srs of state.coneSeries) { try { state.chart.removeSeries(srs); } catch (e) {} }
    state.coneSeries = [];
    for (const srs of state.compareSeries) { try { state.chart.removeSeries(srs); } catch (e) {} }
    state.compareSeries = [];
    removePanes();
  }

  // Comparison lines share the right scale with the price series; the scale is put in
  // percentage mode so the symbols are comparable, and back to normal without them.
  function renderCompare(intraday) {
    const list = state.compare || [];
    state.chart.priceScale('right').applyOptions({ mode: list.length ? LightweightCharts.PriceScaleMode.Percentage : LightweightCharts.PriceScaleMode.Normal });
    for (const c of list) {
      if (!c.points || !c.points.length) continue;
      const srs = state.chart.addLineSeries({ color: c.color, lineWidth: 2, priceLineVisible: false, lastValueVisible: true, title: c.symbol, crosshairMarkerVisible: false });
      srs.setData(c.points.map(p => ({ time: toTime(p.t, intraday), value: p.c })));
      state.compareSeries.push(srs);
    }
  }
  function compareChange(c, hoverT) {
    // % change of a comparison symbol from its first point to the hovered (or last) point.
    if (!c.points || !c.points.length) return null;
    const first = c.points[0].c;
    let p = c.points[c.points.length - 1];
    if (hoverT !== undefined && hoverT !== null) { const hit = c.points.find(x => x.t === hoverT); if (hit) p = hit; }
    return first ? (p.c - first) / first * 100 : null;
  }

  // The implied move cone: ±1 sd solid and ±2 sd dashed bands from the last bar forward,
  // the event marked on the upper band, and the beat / miss targets as axis labels.
  function eventCaptionEl() {
    let el = document.getElementById('eventCaption');
    if (!el) { el = document.createElement('div'); el.id = 'eventCaption'; container.appendChild(el); }
    return el;
  }
  function renderEventCone() {
    const cone = state.eventCone, theme = state.theme;
    const captionEl = eventCaptionEl();
    captionEl.textContent = '';
    captionEl.style.display = 'none';
    const active = cone && cone.enabled && state.main && Array.isArray(cone.points) && cone.points.length >= 2 && (cone.symbol || '') === (state.meta.symbol || '');
    state.chart.timeScale().applyOptions({ rightOffset: 4 });
    if (!active) { state.coneFitFor = null; return; }
    const intraday = !!state.meta.intraday;
    const pts = cone.points.map(p => ({ t: toTime(p.t, intraday), p: p }));
    const mk = (key, color, width, style) => {
      const srs = state.chart.addLineSeries({ color: color, lineWidth: width, lineStyle: style, priceLineVisible: false, lastValueVisible: false, crosshairMarkerVisible: false });
      srs.setData(pts.map(x => ({ time: x.t, value: x.p[key] })));
      state.coneSeries.push(srs);
      return srs;
    };
    const up1 = mk('up1', theme.up, 2, 0);
    mk('dn1', theme.down, 2, 0);
    mk('up2', alpha(theme.up, 0.55), 1, 2);
    mk('dn2', alpha(theme.down, 0.55), 1, 2);
    if (cone.event && cone.event.t !== undefined) {
      const et = toTime(cone.event.t, intraday);
      const target = pts.find(x => sameTime(x.t, et)) || pts[pts.length - 1];
      // Room for the marker text past the last cone point.
      state.chart.timeScale().applyOptions({ rightOffset: 12 });
      up1.setMarkers([{ time: target.t, position: 'aboveBar', color: theme.accent2, shape: 'circle', text: cone.event.label || 'Earnings' }]);
      if (cone.event.up1 && cone.event.dn1) {
        state.main.createPriceLine({ price: cone.event.up1, color: theme.up, lineWidth: 1, lineStyle: 3, axisLabelVisible: true, title: cone.event.up || 'beat' });
        state.main.createPriceLine({ price: cone.event.dn1, color: theme.down, lineWidth: 1, lineStyle: 3, axisLabelVisible: true, title: cone.event.down || 'miss' });
      }
    }
    if (cone.caption) { captionEl.textContent = cone.caption; captionEl.style.display = 'block'; }
    // First time the cone appears for this symbol: fit bars and cone together after render()
    // has restored the range; later re-renders keep the user's zoom.
    if (state.coneFitFor !== (state.meta.symbol || '')) {
      state.coneFitFor = state.meta.symbol || '';
      state.coneNeedsFit = true;
    }
  }

  function seriesData(values) {
    const out = [];
    for (let i = 0; i < state.bars.length; i++) {
      const v = values ? values[i] : null;
      out.push(v === null || v === undefined ? { time: timeOf(i) } : { time: timeOf(i), value: v });
    }
    return out;
  }
  function lineStyleFor(style) { return style === 'dash' ? 2 : style === 'dot' ? 1 : style === 'dots' ? 3 : 0; }
  // One chart series per indicator output, styled from TA-Lib's display hint.
  function addOutputSeries(chart, out, theme, opts) {
    const base = Object.assign({ priceLineVisible: false, lastValueVisible: !!opts.lastValue, crosshairMarkerVisible: false }, opts.extra || {});
    if (out.style === 'hist') {
      const srs = chart.addHistogramSeries(Object.assign({ base: 0 }, base, { lastValueVisible: false }));
      srs.setData(state.bars.map((b, i) => {
        const v = out.values ? out.values[i] : null; const t = timeOf(i);
        return (v === null || v === undefined) ? { time: t } : { time: t, value: v, color: alpha(v >= 0 ? theme.up : theme.down, 0.65) };
      }));
      return srs;
    }
    const srs = chart.addLineSeries(Object.assign({ color: out.color, lineWidth: out.style === 'dots' ? 1 : 1, lineStyle: lineStyleFor(out.style) }, base));
    srs.setData(seriesData(out.values));
    return srs;
  }

  function renderIndicators() {
    const theme = state.theme;
    const markers = [];
    for (const ind of state.indicators) {
      if (!ind || !Array.isArray(ind.outputs)) continue;
      if (ind.placement === 'overlay') {
        ind.series = ind.outputs.map(o => addOutputSeries(state.chart, o, theme, { lastValue: false }));
        state.overlays.push(ind);
      } else if (ind.placement === 'markers') {
        // Candlestick patterns: +100 bullish below the bar, -100 bearish above it.
        const o = ind.outputs[0];
        if (!o || !o.values) continue;
        for (let i = 0; i < state.bars.length && i < o.values.length; i++) {
          const v = o.values[i];
          if (!v) continue;
          markers.push({ i: i, time: timeOf(i), position: v > 0 ? 'belowBar' : 'aboveBar', color: v > 0 ? theme.up : theme.down, shape: v > 0 ? 'arrowUp' : 'arrowDown', text: ind.shortLabel || ind.label });
        }
      } else if (ind.placement === 'pane') {
        const pane = createPane('ind' + ind.id, ind.label);
        pane.indicator = ind;
        const precision = ind.precision === undefined ? 3 : ind.precision;
        const fmtOpts = { priceFormat: { type: 'price', precision: precision, minMove: Math.pow(10, -precision) } };
        ind.series = ind.outputs.map(o => addOutputSeries(pane.chart, o, theme, { lastValue: o.style !== 'hist', extra: fmtOpts }));
        const anchorIndex = ind.outputs.findIndex(o => o.style !== 'hist');
        const anchor = ind.series[anchorIndex >= 0 ? anchorIndex : 0];
        if (anchor && ind.outputs.some(o => o.zero || o.negative)) {
          anchor.createPriceLine({ price: 0, color: alpha(theme.text, 0.35), lineWidth: 1, lineStyle: 3, axisLabelVisible: false });
        }
        // Reference levels for bounded oscillators (e.g. RSI 30/70) supplied by the host.
        if (anchor) for (const lvl of (ind.levels || [])) anchor.createPriceLine({ price: lvl, color: alpha(theme.text, 0.3), lineWidth: 1, lineStyle: 2, axisLabelVisible: false });
        const range = state.chart.timeScale().getVisibleLogicalRange();
        if (range) syncPaneRange(range);
      }
    }
    if (markers.length && state.main) {
      // Label the arrows only when there are few of them (and only when several patterns are
      // on so the label tells them apart); otherwise the names would blanket the bars.
      markers.sort((a, b) => a.i - b.i);
      const patternCount = state.indicators.filter(x => x && x.placement === 'markers').length;
      const withText = markers.length <= 12 && patternCount > 1;
      state.main.setMarkers(markers.map(m => ({ time: m.time, position: m.position, color: m.color, shape: m.shape, text: withText ? m.text : undefined })));
    }
    // Only the bottom pane shows the time axis; the others would repeat it.
    state.panes.forEach((pane, k) => pane.chart.applyOptions({ timeScale: { visible: k === state.panes.length - 1 } }));
    updatePaneLegends(null);
  }

  function render(fit) {
    ensureChart();
    applyFonts();
    // Re-creating the series makes the library scroll to the latest point at its current bar
    // spacing, which pushes the early bars off the left edge; keep the user's range instead.
    const keepRange = (!fit && !state.autoFit && state.chart && state.bars.length) ? state.chart.timeScale().getVisibleLogicalRange() : null;
    removeSeries();
    applyLayout();
    const theme = state.theme, opts = state.options, intraday = !!state.meta.intraday;
    state.chart.applyOptions(chartOptions(theme, intraday));
    const bars = opts.type === 'heikin' ? heikinAshi(state.bars) : state.bars;
    empty.style.display = bars.length ? 'none' : 'flex';
    if (!bars.length) { legend.innerHTML = ''; redraw(); return; }

    const data = bars.map(b => ({ time: toTime(b.t, intraday), open: b.o, high: b.h, low: b.l, close: b.c }));
    if (opts.type === 'bars') {
      state.main = state.chart.addBarSeries({ upColor: theme.up, downColor: theme.down, thinBars: false, priceLineVisible: seriesLineVisible() });
      state.main.setData(data);
    } else if (opts.type === 'line') {
      state.main = state.chart.addLineSeries({ color: theme.accent, lineWidth: 2, priceLineVisible: seriesLineVisible() });
      state.main.setData(bars.map(b => ({ time: toTime(b.t, intraday), value: b.c })));
    } else {
      state.main = state.chart.addCandlestickSeries({
        upColor: theme.up, downColor: theme.down, borderUpColor: theme.up, borderDownColor: theme.down,
        wickUpColor: theme.up, wickDownColor: theme.down, priceLineVisible: seriesLineVisible(),
      });
      state.main.setData(data);
    }
    const lay = layout();
    if (opts.volume && lay.vol) {
      state.volume = state.chart.addHistogramSeries({ priceFormat: { type: 'volume' }, priceScaleId: 'vol' });
      state.chart.priceScale('vol').applyOptions({ scaleMargins: lay.vol });
      state.volume.setData(bars.map(b => ({ time: toTime(b.t, intraday), value: b.v, color: alpha(b.c >= b.o ? theme.up : theme.down, 0.45) })));
    }
    renderCompare(intraday);
    renderIndicators();
    renderEventCone();
    applyLiveLine();
    if (fit) {
      // A new symbol or timeframe: back to autoscale so a manually stretched axis never
      // squashes the new bars into a stale price range.
      state.chart.priceScale('right').applyOptions({ autoScale: true });
      state.autoFit = true;
    }
    if (fit || state.autoFit || state.coneNeedsFit) {
      // Untouched view (or the implied move cone just appeared): bars and cone together.
      state.coneNeedsFit = false;
      state.chart.timeScale().fitContent();
    } else if (keepRange) {
      try { state.chart.timeScale().setVisibleLogicalRange(keepRange); } catch (e) {}
    }
    updateAutoButton();
    updateLegend(null);
    redraw();
  }

  function resetView() {
    if (!state.chart) return;
    state.chart.priceScale('right').applyOptions({ autoScale: true });
    try { state.chart.priceScale('vol').applyOptions({ autoScale: true }); } catch (e) {}
    for (const pane of state.panes) { try { pane.chart.priceScale('right').applyOptions({ autoScale: true }); } catch (e) {} }
    updateAutoButton();
    state.chart.timeScale().resetTimeScale();
    render(true);
    setTool('cursor');
  }

  // Exactly one horizontal price line: the live quote when there is one, otherwise the
  // series' own last-close line; none at all when the user switches price lines off.
  function seriesLineVisible() { return state.options.priceLine !== false && !(state.live && state.live.price); }
  function applyLiveLine() {
    if (state.liveLine && state.liveLineSeries) { try { state.liveLineSeries.removePriceLine(state.liveLine); } catch (e) {} }
    state.liveLine = null; state.liveLineSeries = null;
    if (!state.main) return;
    state.main.applyOptions({ priceLineVisible: seriesLineVisible() });
    if (state.options.priceLine === false || !state.live || !state.live.price || !state.bars.length) return;
    state.liveLine = state.main.createPriceLine({ price: state.live.price, color: state.theme.accent2, lineWidth: 1, lineStyle: 2, axisLabelVisible: true, title: 'live' });
    state.liveLineSeries = state.main;
  }

  function fmt(v, digits) { return (v === undefined || v === null || isNaN(v)) ? '–' : Number(v).toFixed(digits === undefined ? 2 : digits); }
  function fmtVolume(v) { return v >= 1e6 ? (v / 1e6).toFixed(2) + 'M' : v >= 1e3 ? (v / 1e3).toFixed(1) + 'k' : fmt(v, 0); }

  function updateLegend(param) {
    if (!state.bars.length) { legend.innerHTML = ''; return; }
    const theme = state.theme, opts = state.options;
    const source = opts.type === 'heikin' ? heikinAshi(state.bars) : state.bars;
    let bar = source[source.length - 1];
    if (param && param.fromPane) {
      // Hover originated in an indicator pane: show that bar's figures.
      const b = source.find(x => x.t === param.fromPane.t) || param.fromPane;
      bar = { t: b.t, o: b.o, h: b.h, l: b.l, c: b.c, v: b.v };
    } else if (param && param.time !== undefined && state.main) {
      const d = param.seriesData.get(state.main);
      const hovered = source.find(b => { const tt = toTime(b.t, !!state.meta.intraday); return typeof tt === 'object' ? (tt.year === param.time.year && tt.month === param.time.month && tt.day === param.time.day) : tt === param.time; });
      if (d && d.open !== undefined) bar = { t: hovered ? hovered.t : bar.t, o: d.open, h: d.high, l: d.low, c: d.close, v: bar.v };
      else if (d && d.value !== undefined) bar = { t: hovered ? hovered.t : bar.t, o: d.value, h: d.value, l: d.value, c: d.value, v: bar.v };
      if (state.volume) { const vd = param.seriesData.get(state.volume); if (vd) bar.v = vd.value; }
    }
    // Change basis: while hovering, the hovered bar versus the bar before it; otherwise the
    // last bar versus the session's previous close (never the first bar of the series).
    let basis = 0;
    if (param && param.time !== undefined && state.main) {
      const idx = source.findIndex(b => b.t === bar.t);
      basis = idx > 0 ? source[idx - 1].c : bar.o;
    } else {
      basis = state.meta.previousClose || (source.length > 1 ? source[source.length - 2].c : bar.o);
    }
    const change = bar.c - basis;
    const pct = basis ? change / basis * 100 : 0;
    const color = change >= 0 ? theme.up : theme.down;
    const typeName = { candles: 'Candles', bars: 'Bars', heikin: 'Heikin-Ashi', line: 'Line' }[opts.type] || '';
    // Compact (shared-space) legend: symbol, timeframe, close and change only; no company
    // name, O/H/L or session note, so small charts stay readable.
    const compact = !!opts.compact;
    let html = '<span class="sym">' + (state.meta.symbol || '') + '</span>';
    if (state.meta.name && !compact) html += '<span class="name">' + state.meta.name + '</span>';
    html += '<span class="tf">' + (state.meta.timeframe || '') + (compact ? '' : ' · ' + typeName) + '</span>';
    if (!compact) html += '<span>O <b>' + fmt(bar.o) + '</b></span><span>H <b>' + fmt(bar.h) + '</b></span><span>L <b>' + fmt(bar.l) + '</b></span>';
    html += '<span>C <b style="color:' + color + '">' + fmt(bar.c) + '</b></span>';
    html += '<span style="color:' + color + '">' + (change >= 0 ? '+' : '') + fmt(change) + ' (' + (change >= 0 ? '+' : '') + fmt(pct) + '%)</span>';
    html += '<span>Vol <b>' + fmtVolume(bar.v || 0) + '</b></span>';
    if (state.live && state.live.price && !(param && param.time !== undefined)) {
      // Live quote shared with the headline (parity-implied or vendor), with the same previous-close basis.
      const prev = state.live.previousClose || basis;
      const lc = state.live.price - prev, lp = prev ? lc / prev * 100 : 0;
      const lcol = lc >= 0 ? theme.up : theme.down;
      html += '<span style="color:' + theme.accent2 + '">Live <b style="color:' + theme.accent2 + '">' + fmt(state.live.price) + '</b></span>';
      html += '<span style="color:' + lcol + '">' + (lc >= 0 ? '+' : '') + fmt(lc) + ' (' + (lc >= 0 ? '+' : '') + fmt(lp) + '%)</span>';
      html += '<span class="asof">' + (state.live.source || '') + (state.live.asOf ? ' ' + state.live.asOf : '') + '</span>';
    }
    // Indicator readouts: the hovered bar's value, otherwise the latest one.
    const hovering = param && param.time !== undefined;
    const hoverIdx = hovering ? indexOfTime(param.time) : -1;
    for (const ind of state.overlays) {
      if (!ind.outputs.length) continue;
      html += '<span style="color:' + ind.outputs[0].color + '">' + ind.label;
      for (const o of ind.outputs) {
        html += ' <b style="color:' + o.color + '">' + (ind.outputs.length > 1 ? o.short + ' ' : '') + fmt(valueAt(o.values, hoverIdx), ind.precision === undefined ? 2 : ind.precision) + '</b>';
      }
      html += '</span>';
    }
    // Comparison symbols: % change from the first charted point to the hovered (or last) one;
    // the price series' own figure on the same basis sits first so the group reads as a race.
    if (state.compare && state.compare.length && source.length) {
      const base = source[0].c;
      const mainPct = base ? (bar.c - base) / base * 100 : null;
      html += '<span class="tf">vs</span><span style="color:' + theme.accent + '">' + (state.meta.symbol || '') + ' <b>' + (mainPct === null ? '–' : (mainPct >= 0 ? '+' : '') + fmt(mainPct) + '%') + '</b></span>';
      for (const c of state.compare) {
        const pct = compareChange(c, hovering ? bar.t : null);
        html += '<span style="color:' + c.color + '">' + c.symbol + ' <b>' + (pct === null ? '–' : (pct >= 0 ? '+' : '') + fmt(pct) + '%') + '</b></span>';
      }
    }
    if (state.meta.asOf && !compact) html += '<span class="asof">' + state.meta.asOf + '</span>';
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
    // The time axis is hidden when indicator panes are shown, so measure the plot area from
    // the container and the price axis rather than from the time scale widget.
    const ts = state.chart.timeScale();
    let axis = 0;
    try { axis = state.chart.priceScale('right').width() || 0; } catch (e) {}
    const w = axis ? container.clientWidth - axis : ts.width();
    const h = container.clientHeight - (wantedPanes().length ? 0 : ts.height());
    return { w: w, h: h };
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
        if (Math.hypot(x - p.x1, y - p.y1) <= 9) return { d: d, part: 'a' };
        if (Math.hypot(x - p.x2, y - p.y2) <= 9) return { d: d, part: 'b' };
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
        let hit = distToSegment(x, y, p.x1, p.y1, p.x2, p.y2) <= 8;
        if (!hit && d.extend !== false) { const r = rayEnd(p, pane.w); if (r) hit = distToSegment(x, y, r.sx, r.sy, r.ex, r.ey) <= 8; }
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

  // ---- Right-click menu on a drawing (works in every tool mode; the event bubbles from
  // the chart or the overlay to the document) ----
  const menu = document.getElementById('menu');
  let menuTarget = null;
  function drawingLabel(d) { return d.type === 'trend' ? 'trend line' : (d.kind === 'support' ? 'support zone' : 'resistance zone'); }
  function hideMenu() { menu.style.display = 'none'; menuTarget = null; }
  function styleMenu(theme) {
    menu.style.background = theme.bg; menu.style.borderColor = theme.border; menu.style.color = theme.text;
    menu.style.setProperty('--menu-hover', theme.accent);
  }
  function showMenu(clientX, clientY, d) {
    menuTarget = d;
    let html = '';
    if (d) html += '<div class="item" data-act="delete">Delete ' + drawingLabel(d) + '</div>';
    if (D.items.length > (d ? 1 : 0)) html += '<div class="item" data-act="clear">Delete all drawings (' + D.items.length + ')</div>';
    if (html) html += '<div class="sep"></div>';
    html += '<div class="item" data-act="reset">Reset chart view</div>';
    html += '<div class="item" data-act="priceline">' + (state.options.priceLine === false ? 'Show price line' : 'Hide price line') + '</div>';
    html += '<div class="sep"></div><div class="item" data-act="cancel">Cancel</div>';
    menu.innerHTML = html;
    menu.style.display = 'block';
    menu.style.left = Math.max(0, Math.min(clientX, window.innerWidth - menu.offsetWidth - 4)) + 'px';
    menu.style.top = Math.max(0, Math.min(clientY, window.innerHeight - menu.offsetHeight - 4)) + 'px';
  }
  function removeDrawing(id) { D.items = D.items.filter(d => d.id !== id); if (D.selected === id) D.selected = null; emitDrawings(); redraw(); }
  menu.addEventListener('mousedown', e => e.stopPropagation());
  menu.addEventListener('contextmenu', e => e.preventDefault());
  menu.addEventListener('click', e => {
    const act = e.target && e.target.getAttribute ? e.target.getAttribute('data-act') : null;
    if (act === 'delete' && menuTarget) removeDrawing(menuTarget.id);
    else if (act === 'clear') { D.items = []; D.selected = null; emitDrawings(); redraw(); }
    else if (act === 'reset') resetView();
    else if (act === 'priceline') { state.options.priceLine = state.options.priceLine === false; applyLiveLine(); notify('options', { priceLine: state.options.priceLine }); }
    hideMenu();
  });
  // Right-click handling. Chromium normally dispatches `contextmenu`; some host setups only
  // deliver the raw right-button events, so both paths lead here (de-duplicated by time).
  let lastMenuAt = 0;
  function openMenuAt(e) {
    const now = Date.now();
    if (now - lastMenuAt < 350) return true;
    if (!state.bars.length || !state.main) return false;
    const p = pos(e), pane = paneRect();
    if (p.x < 0 || p.y < 0 || p.x > pane.w || p.y > pane.h) return false;
    const hit = hitTest(p.x, p.y, pane);
    lastMenuAt = now;
    D.selected = hit ? hit.d.id : null; redraw();
    showMenu(e.clientX, e.clientY, hit ? hit.d : null);
    notify('menu', hit ? drawingLabel(hit.d) : 'chart');
    return true;
  }
  document.addEventListener('contextmenu', e => {
    if (menu.contains(e.target)) { e.preventDefault(); return; }
    hideMenu();
    if (openMenuAt(e)) { e.preventDefault(); e.stopPropagation(); }
  }, true);
  document.addEventListener('mousedown', e => {
    if (menu.style.display === 'block' && !menu.contains(e.target)) hideMenu();
    if (e.button === 2 && !menu.contains(e.target)) { if (openMenuAt(e)) { e.preventDefault(); e.stopPropagation(); } }
  }, true);
  document.addEventListener('auxclick', e => { if (e.button === 2 && menu.style.display !== 'block') openMenuAt(e); }, true);
  window.addEventListener('blur', hideMenu);
  window.addEventListener('keydown', e => { if (e.key === 'Escape') hideMenu(); }, true);

  function screenshotImpl() {
    if (!state.chart || !state.bars.length) return '';
    // takeScreenshot() paints the chart at its current size, which also refreshes the price
    // scale's coordinate mapping; the drawing overlay is redrawn after that so a resize
    // (pane expanded, window changed) that has not been painted yet still exports correctly.
    const shot = state.chart.takeScreenshot();
    redraw();
    if (!D.items.length && !state.panes.length) return shot.toDataURL('image/png');
    // Main chart with its drawings, then each pane below with a frame, at the screenshot's pixel ratio.
    const scale = shot.width / Math.max(1, container.clientWidth);
    const paneShots = state.panes.map(p => p.chart.takeScreenshot());
    const gap = Math.round(PANE_GAP * scale);
    const out = document.createElement('canvas');
    out.width = shot.width;
    out.height = shot.height + paneShots.reduce((h, s) => h + s.height + gap, 0);
    const ctx = out.getContext('2d');
    ctx.fillStyle = state.theme.bg; ctx.fillRect(0, 0, out.width, out.height);
    ctx.drawImage(shot, 0, 0);
    ctx.drawImage(draw, 0, 0, draw.width, draw.height, 0, 0, shot.width, shot.height);
    let y = shot.height;
    paneShots.forEach((s, i) => {
      y += gap;
      ctx.drawImage(s, 0, y);
      ctx.strokeStyle = state.theme.border; ctx.lineWidth = Math.max(1, Math.round(scale));
      ctx.strokeRect(0.5, y + 0.5, s.width - 1, s.height - 1);
      // Pane legend text is HTML; repeat it on the image.
      ctx.font = Math.round(11 * scale) + 'px Menlo, "SF Mono", monospace'; ctx.fillStyle = state.theme.text;
      ctx.fillText(state.panes[i].legendEl.innerText.replace(/\s+/g, ' '), Math.round(10 * scale), y + Math.round(16 * scale));
      y += s.height;
    });
    return out.toDataURL('image/png');
    }

  window.chartApi = {
    init: function (theme) { state.theme = theme; document.body.style.background = theme.bg; styleMenu(theme); ensureChart(); render(true); },
    setTheme: function (theme) { state.theme = theme; document.body.style.background = theme.bg; styleMenu(theme); if (state.chart) render(false); },
    setBars: function (payload) {
      const meta = payload.meta || {};
      if ((meta.symbol || '') !== (state.meta.symbol || '')) { D.items = []; D.selected = null; D.pending = null; state.live = null; }
      state.bars = payload.bars || []; state.meta = meta; render(true);
    },
    setOptions: function (opts) { state.options = Object.assign(state.options, opts); if (state.chart) render(false); },
    // Indicator results from the host (see state.indicators); re-renders without changing the zoom.
    setIndicators: function (list) { state.indicators = Array.isArray(list) ? list : []; if (state.chart) render(false); },
    // Implied move cone from the option market (null hides it); re-renders without changing the zoom.
    setEventCone: function (spec) { state.eventCone = spec || null; if (state.chart) render(false); },
    // Comparison overlays [{symbol, color, points:[{t, c}]}] (empty list = none); re-renders without changing the zoom.
    setCompare: function (list) { state.compare = Array.isArray(list) ? list : []; if (state.chart) render(false); },
    // Restores autoscale, the default zoom/pan and the cursor tool; bars are re-rendered.
    reset: resetView,
    // Live quote for the charted symbol: {price, previousClose, source, asOf} or null.
    setLive: function (live) { state.live = live || null; if (state.chart && state.main) { applyLiveLine(); updateLegend(null); } },
    screenshot: function () {
      try { return screenshotImpl(); } catch (e) { notify('log', 'screenshot failed: ' + (e && e.message ? e.message : e)); return ''; }
    },
    barCount: function () { return state.bars.length; },
    // Test hook / UI action: expand or restore the indicator pane; returns its height in px (0 without panes).
    togglePaneExpanded: function () { if (!state.panes.length) return 0; toggleExpanded(); return paneHeight(); },
    // Drawing tools
    setTool: setTool,
    setDrawings: function (items) {
      D.items = Array.isArray(items) ? items : []; D.selected = null; D.pending = null;
      D.nextId = D.items.reduce((m, d) => Math.max(m, d.id || 0), 0) + 1;
      redraw();
    },
    drawings: function () { return JSON.stringify(D.items); },
    // Programmatic drawing (assistant tools): {type:'trend', t1, p1, t2, p2, extend} with
    // epoch-second times, or {type:'zone', kind:'support'|'resistance', lo, hi}.
    addDrawing: function (spec) {
      if (!state.bars.length || !spec) return -1;
      const nearest = t => { const l = logicalFromAnchor({ t: t, extra: 0 }); return anchorFromLogical(l); };
      let d = null;
      if (spec.type === 'trend') {
        d = { id: D.nextId++, type: 'trend', a: nearest(spec.t1), pa: spec.p1, b: nearest(spec.t2), pb: spec.p2, extend: spec.extend !== false };
      } else if (spec.type === 'zone') {
        d = { id: D.nextId++, type: 'zone', kind: spec.kind === 'resistance' ? 'resistance' : 'support', lo: Math.min(spec.lo, spec.hi), hi: Math.max(spec.lo, spec.hi) };
      }
      if (!d) return -1;
      D.items.push(d); emitDrawings(); redraw();
      return D.items.length;
    },
    undoDrawing: function () { if (D.items.length) { D.items.pop(); D.selected = null; emitDrawings(); redraw(); } },
    clearDrawings: function () { if (D.items.length) { D.items = []; D.selected = null; emitDrawings(); redraw(); } },
    deleteSelected: deleteSelected,
    // Test hook: draws a trend line and two zones through the real mouse handlers.
    simulateDrawings: function () {
      if (!state.bars.length || !state.main) return 0;
      // Stash whatever the user has drawn (unless a test already did); restoreDrawings() puts it back.
      if (!D.stash) D.stash = D.items.slice();
      D.items = []; D.selected = null;
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
    // Test hook: right-click the first drawing (through the real handlers) and pick Delete.
    simulateContextDelete: function () {
      // Negative results say why: -2 nothing to delete, -3 no pixel position, -4 point outside
      // the plot area, -5 the menu did not open, -6 no Delete entry (nothing was hit), -7 the
      // click did not remove it.
      const target = D.items.find(d => d.type === 'trend') || D.items[0];
      if (!target || !state.main) return -2;
      const pane = paneRect();
      let x, y;
      if (target.type === 'trend') { const p = trendPoints(target); if (!p) return -3; x = (p.x1 + p.x2) / 2; y = (p.y1 + p.y2) / 2; }
      else { x = pane.w / 2; y = yOfPrice((target.lo + target.hi) / 2); }
      if (x === null || y === null || isNaN(x) || isNaN(y)) return -3;
      if (x < 0 || y < 0 || x > pane.w || y > pane.h) { notify('log', 'contextDelete: point outside plot x=' + x + ' y=' + y + ' pane=' + pane.w + 'x' + pane.h + ' range=' + JSON.stringify(state.chart.timeScale().getVisibleLogicalRange())); return -4; }
      const r = draw.getBoundingClientRect();
      const before = D.items.length;
      lastMenuAt = 0;
      container.dispatchEvent(new MouseEvent('contextmenu', { clientX: r.left + x, clientY: r.top + y, bubbles: true, cancelable: true, button: 2 }));
      if (menu.style.display !== 'block') return -5;
      const item = menu.querySelector('[data-act="delete"]');
      if (!item) { hideMenu(); return -6; }
      item.click();
      return D.items.length === before - 1 ? D.items.length : -7;
    },
    stashDrawings: function () {
      if (!D.stash) D.stash = D.items.slice();
      D.items = []; D.selected = null; redraw();
      return D.stash.length;
    },
    restoreDrawings: function () {
      if (!D.stash) return D.items.length;
      D.items = D.stash; D.stash = null; D.selected = null;
      emitDrawings(); redraw();
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
  #chart { position: absolute; left: 0; right: 0; top: 0; bottom: 0; }
  #draw { position: absolute; left: 0; top: 0; z-index: 4; pointer-events: none; }
  #panes { position: absolute; left: 0; right: 0; bottom: 0; display: none; }
  .pane { position: relative; box-sizing: border-box; border: 1px solid #273449; border-radius: 4px; overflow: hidden; }
  .pane-chart { position: absolute; inset: 0; }
  .pane-legend { position: absolute; left: 10px; top: 4px; z-index: 5; pointer-events: none; display: flex; gap: 0 12px;
                 font: var(--pane-legend-size, 11px) Menlo, "SF Mono", monospace; color: #c7d2e3; line-height: 18px; }
  .pane-legend .title { font-weight: 700; }
  .pane-legend b { font-weight: 600; }
  .pane-btn { position: absolute; right: 86px; top: 3px; z-index: 6; width: 20px; height: 18px; line-height: 18px; text-align: center;
              border: 1px solid #273449; border-radius: 4px; background: rgba(18,26,43,0.85); color: #8294ad; font-size: 12px; cursor: pointer; user-select: none; }
  .pane-btn:hover { color: #f3f6fb; border-color: #3b82f6; }
  #pane-handle { position: absolute; left: 0; right: 0; height: 8px; z-index: 7; display: none; cursor: ns-resize; }
  #pane-handle::after { content: ''; position: absolute; left: 50%; top: 3px; width: 36px; height: 2px; margin-left: -18px; border-radius: 1px;
                        background: rgba(130,148,173,0.45); }
  #pane-handle:hover::after, #pane-handle.active::after { background: #3b82f6; }
  #auto-scale { position: absolute; right: 6px; bottom: 34px; z-index: 6; display: none; padding: 0 6px; height: 18px; line-height: 18px;
                border: 1px solid #273449; border-radius: 4px; background: rgba(18,26,43,0.9); color: #8294ad;
                font: 11px -apple-system, "Helvetica Neue", sans-serif; cursor: pointer; user-select: none; }
  #auto-scale:hover { color: #f3f6fb; border-color: #3b82f6; }
  #menu { position: absolute; z-index: 20; display: none; min-width: 190px; padding: 4px 0; border: 1px solid #273449; border-radius: 6px;
          background: #121a2b; color: #c7d2e3; font: 12px -apple-system, "Helvetica Neue", sans-serif; box-shadow: 0 8px 24px rgba(0,0,0,0.45);
          --menu-hover: #3b82f6; user-select: none; }
  #menu .item { padding: 6px 14px; cursor: pointer; white-space: nowrap; }
  #menu .item:hover { background: var(--menu-hover); color: #ffffff; }
  #menu .sep { height: 1px; margin: 4px 0; background: rgba(130,148,173,0.35); }
  #legend { position: absolute; left: 12px; top: 8px; z-index: 5; pointer-events: none;
            font: var(--legend-size, 12px) Menlo, "SF Mono", monospace; color: #c7d2e3; display: flex; flex-wrap: wrap; gap: 0 14px; line-height: var(--legend-line, 20px); }
  #legend .sym { font-weight: 700; font-size: var(--sym-size, 14px); color: #f59e0b; }
  #legend .name, #legend .tf, #legend .asof { color: #8294ad; }
  #legend b { color: #f3f6fb; font-weight: 600; }
  #eventCaption { position: absolute; left: 12px; bottom: 34px; z-index: 5; pointer-events: none; display: none;
                  font: 11px Menlo, "SF Mono", monospace; color: #f59e0b; background: rgba(15, 23, 42, 0.72); padding: 2px 7px; border-radius: 4px; }
  #empty { position: absolute; inset: 0; z-index: 6; display: flex; align-items: center; justify-content: center;
           font: 13px -apple-system, sans-serif; color: #8294ad; }
</style></head>
<body>
<div id="chart"></div>
<div id="panes"></div>
<div id="pane-handle" title="Drag to resize the indicator pane; double-click to expand or restore"></div>
<div id="auto-scale" title="The price axis has been scaled by hand; click to return to automatic scaling">Auto</div>
<canvas id="draw"></canvas>
<div id="menu"></div>
<div id="legend"></div>
<div id="empty">Select a ticker in the watchlist to load its chart.</div>
<script>%1</script>
<script>%2</script>
</body></html>)html").arg(QString::fromUtf8(kLightweightChartsJs), QString::fromUtf8(kControllerJs));
    return html;
}
