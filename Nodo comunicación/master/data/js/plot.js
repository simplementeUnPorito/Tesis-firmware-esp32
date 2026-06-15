// plot.js — canvas-based multi-channel live plotting. Mirrors gui/plot_area.py
// (PlotArea/pyqtgraph): one stacked plot per node slot, raw (blue) + filtered
// (red, only while configured) curves, auto-ranged Y axis, node visibility/
// order control. Plain <canvas> + 2D context — no charting library, keeps the
// phone payload small and the edit→uploadfs→reload loop dependency-free.

import * as cfg from './config.js?v=field-loop-20';

const RAW_COLOR = 'rgb(80, 140, 255)';
const FILT_COLOR = 'rgb(255, 80, 80)';
const CURSOR_STYLES = [
  { label: 'C1', line: 'rgba(255,230,80,0.9)', stroke: 'rgba(255,230,80,0.65)', text: '#ffe980' },
  { label: 'C2', line: 'rgba(80,220,255,0.9)', stroke: 'rgba(80,220,255,0.65)', text: '#80eaff' },
];

const MARGIN = { left: 46, right: 10, top: 20, bottom: 20 };

/** "Nice" rounded tick step (Heckbert's algorithm, simplified to one decade lookup). */
function niceNum(range, round) {
  if (!(range > 0)) return 1;
  const exp = Math.floor(Math.log10(range));
  const frac = range / 10 ** exp;
  let niceFrac;
  if (round) niceFrac = frac < 1.5 ? 1 : frac < 3 ? 2 : frac < 7 ? 5 : 10;
  else niceFrac = frac <= 1 ? 1 : frac <= 2 ? 2 : frac <= 5 ? 5 : 10;
  return niceFrac * 10 ** exp;
}

/** Evenly spaced "nice" tick values covering at least [lo, hi]. */
function niceTicks(lo, hi, count) {
  if (lo === hi) { lo -= 0.5; hi += 0.5; }
  const step = niceNum(niceNum(hi - lo, false) / Math.max(1, count - 1), true);
  const niceLo = Math.floor(lo / step) * step;
  const niceHi = Math.ceil(hi / step) * step;
  const ticks = [];
  for (let v = niceLo; v <= niceHi + step / 2; v += step) ticks.push(v);
  return ticks;
}

function fmtVolt(v) {
  const av = Math.abs(v);
  if (av !== 0 && (av < 1e-3 || av >= 1e4)) return v.toExponential(1);
  if (av >= 100) return v.toFixed(0);
  if (av >= 1) return v.toFixed(2);
  return v.toFixed(3);
}

function minMax(arr, start = 0, end = arr.length) {
  let lo = Infinity, hi = -Infinity;
  const from = Math.max(0, start | 0);
  const to = Math.min(arr.length, Math.max(from, end | 0));
  for (let i = from; i < to; i++) {
    const v = arr[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return Number.isFinite(lo) && Number.isFinite(hi) ? { lo, hi } : null;
}

function cssVar(el, name, fallback) {
  const value = getComputedStyle(el).getPropertyValue(name).trim();
  return value || fallback;
}

function clamp(value, lo, hi) {
  return Math.max(lo, Math.min(hi, value));
}

/** One stacked channel: canvas + raw/filtered curves, grid, title, legend. */
class ChannelPlot {
  constructor(container, title, handlers = {}) {
    this.title = title;
    this._raw = null;    // Float64Array | null — already windowed to the display tail
    this._filt = null;   // Float64Array | null
    this._fs = 0;        // 0 = Fs not yet reported by hardware — don't draw a time axis
    this._xStartSamp = 0;
    this._xSpanSamp = cfg.DISP_SAMP;
    this._visible = true;
    this._showRaw = true;
    this._showFilt = true;
    this._handlers = handlers;
    this._dragLastX = null;
    this._cursorMode = null;
    this._cursorSamps = [null, null];

    this.wrap = document.createElement('div');
    this.wrap.className = 'plot-wrap';
    this.canvas = document.createElement('canvas');
    this.wrap.appendChild(this.canvas);
    container.appendChild(this.wrap);

    this._ctx = this.canvas.getContext('2d');
    this._ro = new ResizeObserver(() => this._fitCanvas());
    this._ro.observe(this.wrap);
    this._wirePointerControls();
    this._fitCanvas();
  }

  setVisible(visible) {
    this._visible = visible;
    this.wrap.style.display = visible ? '' : 'none';
    if (visible) this._fitCanvas();
  }

  get visible() { return this._visible; }

  setTitle(title) {
    this.title = title || this.title;
  }

  /** Show/hide the raw and/or filtered curve independently of whether data exists for them. */
  setCurveVisibility(showRaw, showFilt) {
    this._showRaw = showRaw !== false;
    this._showFilt = showFilt !== false;
  }

  /** raw/filt: Float64Array|null, already windowed. fs: Hz, for the time axis.
   *  filtTrimSamp: count of leading filtered samples to discard before
   *  drawing (0 → keep all). See PlotArea.update for why. */
  setData(raw, filt, fs, xStartSamp, xSpanSamp, filtTrimSamp = 0) {
    this._raw = (raw && raw.length) ? raw : null;
    this._filt = (filt && filt.length) ? filt : null;
    // No nominal fallback: Fs comes only from the hardware HELLO. 0 = unknown.
    this._fs = fs || 0;
    this._xStartSamp = Math.max(0, xStartSamp | 0);
    this._xSpanSamp = Math.max(1, xSpanSamp | 0);
    this._filtTrimSamp = filtTrimSamp || 0;
  }

  setCursors(samps) {
    this._cursorSamps = CURSOR_STYLES.map((_, i) => {
      const samp = samps && samps[i];
      return (Number.isFinite(samp) && samp >= 0) ? samp : null;
    });
  }

  setCursorMode(mode) {
    this._cursorMode = Number.isInteger(mode) && mode >= 0 && mode < CURSOR_STYLES.length
      ? mode
      : null;
    this.canvas.style.cursor = this._cursorMode !== null ? 'crosshair' : '';
  }

  clear() {
    this._raw = null;
    this._filt = null;
  }

  _fitCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const rect = this.wrap.getBoundingClientRect();
    const w = Math.max(1, Math.round(rect.width * dpr));
    const h = Math.max(1, Math.round(rect.height * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
    }
  }

  _wirePointerControls() {
    this.canvas.addEventListener('wheel', (ev) => {
      if (!this._handlers.onZoom) return;
      ev.preventDefault();
      const rect = this.canvas.getBoundingClientRect();
      const frac = clamp((ev.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
      const factor = ev.deltaY < 0 ? 1.25 : 1 / 1.25;
      this._handlers.onZoom(factor, frac);
    }, { passive: false });

    // A press that doesn't move (beyond CLICK_SLOP) is a click-to-zoom:
    // left button (or plain tap) zooms in, right button or shift-click zooms
    // out, anchored at the clicked x position. A press that moves pans, same
    // as before. This replaces dblclick-to-reset (the toolbar Reset button
    // covers that) since the two gestures would otherwise both fire on a
    // double-click and fight each other.
    const CLICK_SLOP = 4;
    this.canvas.addEventListener('pointerdown', (ev) => {
      this._dragLastX = ev.clientX;
      this._downX = ev.clientX;
      this._downY = ev.clientY;
      this._downButton = ev.button;
      this._dragMoved = false;
      this.canvas.setPointerCapture(ev.pointerId);
    });
    this.canvas.addEventListener('pointermove', (ev) => {
      if (this._dragLastX === null) return;
      if (!this._dragMoved
          && (Math.abs(ev.clientX - this._downX) > CLICK_SLOP
              || Math.abs(ev.clientY - this._downY) > CLICK_SLOP)) {
        this._dragMoved = true;
      }
      if (!this._handlers.onPan) return;
      const dx = ev.clientX - this._dragLastX;
      this._dragLastX = ev.clientX;
      const rect = this.canvas.getBoundingClientRect();
      this._handlers.onPan(-dx / Math.max(1, rect.width));
    });
    const endDrag = (ev) => {
      if (this._dragLastX !== null && !this._dragMoved) {
        const rect = this.canvas.getBoundingClientRect();
        const frac = clamp((ev.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
        if (this._cursorMode !== null && this._handlers.onCursorClick) {
          this._handlers.onCursorClick(frac);
        } else if (this._handlers.onZoom) {
          const zoomOut = this._downButton === 2 || ev.shiftKey;
          this._handlers.onZoom(zoomOut ? 1 / 1.5 : 1.5, frac);
        }
      }
      this._dragLastX = null;
    };
    this.canvas.addEventListener('pointerup', endDrag);
    this.canvas.addEventListener('pointercancel', () => { this._dragLastX = null; });
    // Right-click drives zoom-out (above) instead of opening a context menu.
    this.canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
  }

  draw() {
    if (!this._visible) return;
    if (!this._fs) return;   // Fs not yet reported — nothing meaningful to plot
    const ctx = this._ctx;
    const dpr = window.devicePixelRatio || 1;
    const W = this.canvas.width;
    const H = this.canvas.height;
    const m = {
      left: MARGIN.left * dpr, right: MARGIN.right * dpr,
      top: MARGIN.top * dpr, bottom: MARGIN.bottom * dpr,
    };
    const plotW = Math.max(1, W - m.left - m.right);
    const plotH = Math.max(1, H - m.top - m.bottom);
    const colors = {
      bg: cssVar(document.body, '--plot-bg', '#000'),
      grid: cssVar(document.body, '--plot-grid', 'rgba(255,255,255,0.12)'),
      border: cssVar(document.body, '--plot-border', 'rgba(255,255,255,0.25)'),
      axis: cssVar(document.body, '--plot-axis', '#9ab'),
      title: cssVar(document.body, '--plot-title', '#ddd'),
    };

    ctx.save();
    ctx.fillStyle = colors.bg;
    ctx.fillRect(0, 0, W, H);

    const raw = this._raw;
    const filt = this._filt;
    const xLo = this._xStartSamp / this._fs;
    const xHi = (this._xStartSamp + this._xSpanSamp) / this._fs;
    const xSpan = Math.max(1 / this._fs, xHi - xLo);

    const showRaw = this._showRaw && raw;
    const showFilt = this._showFilt && filt;
    let yLo = -1, yHi = 1;
    if (showRaw || showFilt) {
      let lo = Infinity, hi = -Infinity;
      if (showRaw) {
        const r = minMax(raw);
        if (r) { lo = Math.min(lo, r.lo); hi = Math.max(hi, r.hi); }
      }
      if (showFilt) {
        const r = minMax(filt, this._filtTrimSamp);
        if (r) { lo = Math.min(lo, r.lo); hi = Math.max(hi, r.hi); }
      }
      if (Number.isFinite(lo) && Number.isFinite(hi)) {
        if (lo === hi) { lo -= 1; hi += 1; }
        const pad = (hi - lo) * 0.08;
        yLo = lo - pad;
        yHi = hi + pad;
      }
    }

    const xTo = (t) => m.left + ((t - xLo) / xSpan) * plotW;
    const yTo = (v) => m.top + plotH - ((v - yLo) / ((yHi - yLo) || 1)) * plotH;

    // Gridlines + axis tick labels
    ctx.lineWidth = Math.max(1, dpr);
    ctx.font = `${Math.round(10 * dpr)}px ui-monospace, monospace`;

    ctx.strokeStyle = colors.grid;
    ctx.fillStyle = colors.axis;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const v of niceTicks(yLo, yHi, 5)) {
      const py = yTo(v);
      if (py < m.top - 1 || py > m.top + plotH + 1) continue;
      ctx.beginPath();
      ctx.moveTo(m.left, py);
      ctx.lineTo(m.left + plotW, py);
      ctx.stroke();
      ctx.fillText(fmtVolt(v), m.left - 6 * dpr, py);
    }

    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (const tMs of niceTicks(xLo * 1000, xHi * 1000, 5)) {
      const t = tMs / 1000;
      if (t < xLo - 1e-9 || t > xHi + 1e-9) continue;
      const px = xTo(t);
      ctx.beginPath();
      ctx.moveTo(px, m.top);
      ctx.lineTo(px, m.top + plotH);
      ctx.stroke();
      const lbl = Math.abs(tMs) >= 10000 ? `${(tMs / 1000).toFixed(1)}s` : `${tMs.toFixed(0)}ms`;
      ctx.fillText(lbl, px, m.top + plotH + 5 * dpr);
    }

    ctx.strokeStyle = colors.border;
    ctx.strokeRect(m.left, m.top, plotW, plotH);

    // Curves (clip to the plot rect so partially-filled buffers don't smear into margins)
    ctx.save();
    ctx.beginPath();
    ctx.rect(m.left, m.top, plotW, plotH);
    ctx.clip();
    const drawCurve = (data, color, trimSamp) => {
      if (!data || data.length < 2 + trimSamp) return;
      ctx.strokeStyle = color;
      ctx.lineWidth = Math.max(1, dpr);
      ctx.beginPath();
      const len = data.length;
      let started = false;
      for (let i = trimSamp; i < len; i++) {
        // filt[trimSamp + k] is the group-delay-compensated sample that lines
        // up with raw[k] (k = i - trimSamp); plot it at raw[k]'s x-position,
        // not at its own raw index, or the curve ends up shifted right by an
        // extra trimSamp samples on top of the filter's own group delay.
        const px = xTo((this._xStartSamp + i - trimSamp) / this._fs);
        const py = yTo(data[i]);
        if (!started) { ctx.moveTo(px, py); started = true; }
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
    };
    if (this._showRaw) drawCurve(raw, RAW_COLOR, 0);
    // Linear-phase FIR of length N has constant group delay (N-1)/2: its
    // first (N-1)/2 outputs are pure startup transient. Discard them so the
    // rest aligns index-for-index with raw, and the transient never appears.
    if (this._showFilt) drawCurve(filt, FILT_COLOR, this._filtTrimSamp);
    ctx.restore();

    // Cursors - vertical dashed lines + readout boxes.
    const deltaMs = (this._cursorSamps[0] !== null && this._cursorSamps[1] !== null)
      ? ((this._cursorSamps[1] - this._cursorSamps[0]) / this._fs) * 1000
      : null;
    let nextBoxY = m.top + 5 * dpr;
    for (let ci = 0; ci < CURSOR_STYLES.length; ci++) {
      const cursorSamp = this._cursorSamps[ci];
      if (cursorSamp === null) continue;
      const cursorT = cursorSamp / this._fs;
      if (cursorT < xLo - 1e-9 || cursorT > xHi + 1e-9) continue;

      const style = CURSOR_STYLES[ci];
      const cpx = xTo(cursorT);
      ctx.save();
      ctx.strokeStyle = style.line;
      ctx.lineWidth = dpr;
      ctx.setLineDash([5 * dpr, 3 * dpr]);
      ctx.beginPath();
      ctx.moveTo(cpx, m.top);
      ctx.lineTo(cpx, m.top + plotH);
      ctx.stroke();
      ctx.setLineDash([]);

      ctx.font = `${Math.round(10 * dpr)}px ui-monospace, monospace`;
      ctx.fillStyle = style.text;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      ctx.fillText(style.label, cpx, m.top + 2 * dpr);

      const relIdx = cursorSamp - this._xStartSamp;
      const tMs = cursorT * 1000;
      const lines = [`${style.label} t: ${tMs.toFixed(2)} ms`];
      if (this._raw && relIdx >= 0 && relIdx < this._raw.length)
        lines.push(`raw: ${fmtVolt(this._raw[relIdx])} V`);
      const filtIdx = relIdx + (this._filtTrimSamp || 0);
      if (this._filt && filtIdx >= 0 && filtIdx < this._filt.length)
        lines.push(`filt: ${fmtVolt(this._filt[filtIdx])} V`);
      if (ci === 1 && deltaMs !== null) lines.push(`dt: ${deltaMs.toFixed(2)} ms`);

      const lineH = 13 * dpr;
      const pad = 5 * dpr;
      const boxW = Math.max(...lines.map((l) => ctx.measureText(l).width)) + pad * 2 + 4 * dpr;
      const boxH = lines.length * lineH + pad * 2;
      let bx = cpx + 8 * dpr;
      if (bx + boxW > m.left + plotW - 2 * dpr) bx = cpx - boxW - 8 * dpr;
      const maxBoxY = Math.max(m.top + pad, m.top + plotH - boxH - 2 * dpr);
      const by = Math.min(nextBoxY, maxBoxY);
      nextBoxY = by + boxH + 4 * dpr;

      ctx.fillStyle = 'rgba(20,20,20,0.85)';
      ctx.strokeStyle = style.stroke;
      ctx.lineWidth = dpr;
      ctx.beginPath();
      if (ctx.roundRect) ctx.roundRect(bx, by, boxW, boxH, 3 * dpr);
      else ctx.rect(bx, by, boxW, boxH);
      ctx.fill();
      ctx.stroke();

      ctx.fillStyle = style.text;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'top';
      for (let li = 0; li < lines.length; li++)
        ctx.fillText(lines[li], bx + pad, by + pad + li * lineH);
      ctx.restore();
    }

    // Title (top-left) + legend (top-right)
    ctx.font = `${Math.round(11 * dpr)}px system-ui, sans-serif`;
    ctx.fillStyle = colors.title;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    ctx.fillText(this.title, m.left, 4 * dpr);

    let lx = W - m.right;
    const minLegendX = m.left + ctx.measureText(this.title).width + 16 * dpr;
    const drawLegendItem = (text, color) => {
      const width = ctx.measureText(text).width;
      if (lx - width < minLegendX) return false;
      ctx.fillStyle = color;
      ctx.fillText(text, lx, 4 * dpr);
      lx -= width + 14 * dpr;
      return true;
    };
    ctx.textAlign = 'right';
    if (filt && this._showFilt) {
      drawLegendItem('Filtrada', FILT_COLOR);
    }
    if (this._showRaw) {
      drawLegendItem('Cruda', RAW_COLOR);
    }

    // Y-axis unit label (rotated)
    ctx.save();
    ctx.font = `${Math.round(9 * dpr)}px ui-monospace, monospace`;
    ctx.fillStyle = colors.axis;
    ctx.translate(9 * dpr, m.top + plotH / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('V', 0, 0);
    ctx.restore();

    ctx.restore();
  }
}

/**
 * Stacked canvas plots — one per active node. Mirrors PlotArea: each shows
 * raw (blue) + filtered (red, only when present) curves, auto-ranged Y,
 * shared/auto time axis, node visibility/ordering, display-window control.
 *
 * Call update() from the render loop (mirrors RENDER_PERIOD_MS / _render).
 */
export class PlotArea {
  constructor(container) {
    this._container = container;
    this._dispSamp = cfg.DISP_SAMP;
    this._zoom = 1;
    this._panSamp = 0;
    this._followTail = true;
    this._showRaw = true;
    this._showFilt = true;
    this._lastMaxLen = 0;
    this._cursorMode = null;
    this._cursorSamps = [null, null];
    this._plots = [];
    const handlers = {
      onZoom: (factor, anchor) => this.zoomBy(factor, anchor),
      onPan: (frac) => this.panByFraction(frac),
      onCursorClick: (frac) => {
        if (this._cursorMode === null) return;
        this._cursorSamps[this._cursorMode] = Math.round(
          this._panSamp + clamp(frac, 0, 1) * this._viewSamples(),
        );
      },
    };
    for (let i = 0; i < cfg.MAX_NODES; i++) {
      this._plots.push(new ChannelPlot(container, cfg.NODE_NAMES[i], handlers));
    }
    // Master slot (index 0) hidden by default — gateway only, not plotted.
    this._plots[0].setVisible(false);
  }

  /** Show/hide the raw and/or filtered curves across every plot (display only — saving always keeps both). */
  setCurveVisibility(showRaw, showFilt) {
    this._showRaw = showRaw !== false;
    this._showFilt = showFilt !== false;
    for (const p of this._plots) {
      p.setCurveVisibility(this._showRaw, this._showFilt);
      if (p.visible) p.draw();
    }
  }

  /** Change the width of the X-axis window, in samples. */
  setDisplaySamples(n) {
    this._dispSamp = Math.max(1, n | 0);
    this.resetView();
  }

  _viewSamples() {
    return Math.max(8, Math.round(this._dispSamp / this._zoom));
  }

  _clampPan() {
    const maxStart = Math.max(0, this._lastMaxLen - this._viewSamples());
    this._panSamp = clamp(Math.round(this._panSamp), 0, maxStart);
  }

  zoomBy(factor, anchorFrac = 0.5) {
    const oldSpan = this._viewSamples();
    const oldStart = this._panSamp;
    this._zoom = clamp(this._zoom * factor, 1, 128);
    const newSpan = this._viewSamples();
    const anchorSamp = oldStart + clamp(anchorFrac, 0, 1) * oldSpan;
    this._panSamp = anchorSamp - clamp(anchorFrac, 0, 1) * newSpan;
    this._followTail = false;
    this._clampPan();
  }

  panByFraction(frac) {
    this._panSamp += frac * this._viewSamples();
    this._followTail = false;
    this._clampPan();
  }

  resetView() {
    this._zoom = 1;
    this._followTail = true;
    this._clampPan();
  }

  setCursorMode(mode) {
    this._cursorMode = Number.isInteger(mode) && mode >= 0 && mode < CURSOR_STYLES.length
      ? mode
      : null;
    for (const p of this._plots) p.setCursorMode(this._cursorMode);
  }

  getCursorSamples() {
    return this._cursorSamps.slice();
  }

  setNodeTitles(titles) {
    for (let i = 0; i < this._plots.length; i++) {
      if (titles && titles[i]) this._plots[i].setTitle(titles[i]);
    }
  }

  /** Show only the given node indices, in the given order (mirrors set_active_nodes). */
  setActiveNodes(nodeIndices) {
    for (const p of this._plots) {
      if (p.wrap.parentNode === this._container) this._container.removeChild(p.wrap);
      p.setVisible(false);
    }
    for (const idx of nodeIndices) {
      if (idx >= 0 && idx < this._plots.length) {
        this._container.appendChild(this._plots[idx].wrap);
        this._plots[idx].setVisible(true);
      }
    }
  }

  /**
   * Refresh all visible plots with the latest buffer contents.
   * @param {Array<Float64Array|null>} rawBufs  full per-node raw sample arrays (index 0..MAX_NODES-1)
   * @param {Array<Float64Array|null>} filtBufs same, filtered (null/empty hides the filt curve)
   * @param {number} [fs]  Sample rate for the shared time axis (mirrors _effective_fs; the
   *                       slave plots are X-linked, so they must share one timescale).
   * @param {Array<number>} [filtTrims] per-node count of leading filtered samples to
   *                       discard before drawing (mirrors _render's filt_trims —
   *                       (N-1)/2 for an N-tap linear-phase FIR, 0 when no filter is
   *                       active). That many samples are the filter's startup
   *                       transient; dropping them both hides that transient and
   *                       re-aligns what's left with raw at the same x positions.
   */
  update(rawBufs, filtBufs, fs, filtTrims) {
    this._lastMaxLen = 0;
    for (let i = 0; i < this._plots.length; i++) {
      const rawLen = rawBufs[i] ? rawBufs[i].length : 0;
      const filtLen = filtBufs[i] ? filtBufs[i].length : 0;
      this._lastMaxLen = Math.max(this._lastMaxLen, rawLen, filtLen);
    }
    const viewSamples = this._viewSamples();
    if (this._followTail) this._panSamp = Math.max(0, this._lastMaxLen - viewSamples);
    this._clampPan();

    for (let i = 0; i < this._plots.length; i++) {
      const p = this._plots[i];
      if (!p.visible) continue;

      const raw = rawBufs[i];
      const filt = filtBufs[i];
      const start = this._panSamp;
      const end = start + viewSamples;
      const rawTail = (raw && raw.length) ? raw.subarray(Math.min(start, raw.length), Math.min(end, raw.length)) : null;

      const filtTrim = (filtTrims && filtTrims[i]) || 0;
      // The filtered curve is drawn as filt[trim + k] at raw[k]'s x-position.
      // Include trim extra samples on the right so the visible filtered trace
      // still reaches rawTail's right edge after drawCurve discards the lead-in.
      const filtEnd = end + filtTrim;
      const filtTail = (filt && filt.length) ? filt.subarray(Math.min(start, filt.length), Math.min(filtEnd, filt.length)) : null;
      // No nominal fallback: fs is 0 until the hardware reports it, and
      // setData/draw treat 0 as "don't draw a time axis yet".
      p.setData(rawTail, filtTail, fs || 0, start, viewSamples, filtTrim);
      p.setCursors(this._cursorSamps);
      p.draw();
    }
  }

  /** Clear every curve to a blank state. */
  clearAll() {
    for (const p of this._plots) { p.clear(); p.draw(); }
  }

  /** Clear the curve for one node immediately. */
  clearNode(idx) {
    if (idx >= 0 && idx < this._plots.length) {
      this._plots[idx].clear();
      this._plots[idx].draw();
    }
  }
}
