// plot.js — canvas-based multi-channel live plotting. Mirrors gui/plot_area.py
// (PlotArea/pyqtgraph): one stacked plot per node slot, raw (blue) + filtered
// (red, only while configured) curves, auto-ranged Y axis, node visibility/
// order control. Plain <canvas> + 2D context — no charting library, keeps the
// phone payload small and the edit→uploadfs→reload loop dependency-free.

import * as cfg from './config.js';

const RAW_COLOR = 'rgb(80, 140, 255)';
const FILT_COLOR = 'rgb(255, 80, 80)';

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

function minMax(arr) {
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < arr.length; i++) {
    const v = arr[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return { lo, hi };
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
    this._fs = cfg.FS;
    this._xStartSamp = 0;
    this._xSpanSamp = cfg.DISP_SAMP;
    this._visible = true;
    this._handlers = handlers;
    this._dragLastX = null;

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

  /** raw/filt: Float64Array|null, already windowed. fs: Hz, for the time axis. */
  setData(raw, filt, fs, xStartSamp, xSpanSamp) {
    this._raw = (raw && raw.length) ? raw : null;
    this._filt = (filt && filt.length) ? filt : null;
    this._fs = fs || cfg.FS;
    this._xStartSamp = Math.max(0, xStartSamp | 0);
    this._xSpanSamp = Math.max(1, xSpanSamp | 0);
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

    this.canvas.addEventListener('pointerdown', (ev) => {
      this._dragLastX = ev.clientX;
      this.canvas.setPointerCapture(ev.pointerId);
    });
    this.canvas.addEventListener('pointermove', (ev) => {
      if (this._dragLastX === null || !this._handlers.onPan) return;
      const dx = ev.clientX - this._dragLastX;
      this._dragLastX = ev.clientX;
      const rect = this.canvas.getBoundingClientRect();
      this._handlers.onPan(-dx / Math.max(1, rect.width));
    });
    const endDrag = () => { this._dragLastX = null; };
    this.canvas.addEventListener('pointerup', endDrag);
    this.canvas.addEventListener('pointercancel', endDrag);
    this.canvas.addEventListener('dblclick', () => {
      if (this._handlers.onReset) this._handlers.onReset();
    });
  }

  draw() {
    if (!this._visible) return;
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

    let yLo = -1, yHi = 1;
    if (raw || filt) {
      let lo = Infinity, hi = -Infinity;
      if (raw)  { const r = minMax(raw);  lo = Math.min(lo, r.lo); hi = Math.max(hi, r.hi); }
      if (filt) { const r = minMax(filt); lo = Math.min(lo, r.lo); hi = Math.max(hi, r.hi); }
      if (lo === hi) { lo -= 1; hi += 1; }
      const pad = (hi - lo) * 0.08;
      yLo = lo - pad;
      yHi = hi + pad;
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
    for (const t of niceTicks(xLo, xHi, 5)) {
      if (t < xLo - 1e-9 || t > xHi + 1e-9) continue;
      const px = xTo(t);
      ctx.beginPath();
      ctx.moveTo(px, m.top);
      ctx.lineTo(px, m.top + plotH);
      ctx.stroke();
      ctx.fillText(`${t.toFixed(t < 1 ? 2 : 1)}s`, px, m.top + plotH + 5 * dpr);
    }

    ctx.strokeStyle = colors.border;
    ctx.strokeRect(m.left, m.top, plotW, plotH);

    // Curves (clip to the plot rect so partially-filled buffers don't smear into margins)
    ctx.save();
    ctx.beginPath();
    ctx.rect(m.left, m.top, plotW, plotH);
    ctx.clip();
    const drawCurve = (data, color) => {
      if (!data || data.length < 2) return;
      ctx.strokeStyle = color;
      ctx.lineWidth = Math.max(1, dpr);
      ctx.beginPath();
      const len = data.length;
      for (let i = 0; i < len; i++) {
        const px = xTo((this._xStartSamp + i) / this._fs);
        const py = yTo(data[i]);
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
    };
    drawCurve(raw, RAW_COLOR);
    drawCurve(filt, FILT_COLOR);
    ctx.restore();

    // Title (top-left) + legend (top-right)
    ctx.font = `${Math.round(11 * dpr)}px system-ui, sans-serif`;
    ctx.fillStyle = colors.title;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    ctx.fillText(this.title, m.left, 4 * dpr);

    let lx = W - m.right;
    ctx.textAlign = 'right';
    if (filt) {
      ctx.fillStyle = FILT_COLOR;
      ctx.fillText('Filtrada', lx, 4 * dpr);
      lx -= ctx.measureText('Filtrada').width + 14 * dpr;
    }
    ctx.fillStyle = RAW_COLOR;
    ctx.fillText('Cruda', lx, 4 * dpr);

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
    this._lastMaxLen = 0;
    this._plots = [];
    const handlers = {
      onZoom: (factor, anchor) => this.zoomBy(factor, anchor),
      onPan: (frac) => this.panByFraction(frac),
      onReset: () => this.resetView(),
    };
    for (let i = 0; i < cfg.MAX_NODES; i++) {
      this._plots.push(new ChannelPlot(container, cfg.NODE_NAMES[i], handlers));
    }
    // Master slot (index 0) hidden by default — gateway only, not plotted.
    this._plots[0].setVisible(false);
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
   */
  update(rawBufs, filtBufs, fs) {
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
      const filtTail = (filt && filt.length) ? filt.subarray(Math.min(start, filt.length), Math.min(end, filt.length)) : null;

      p.setData(rawTail, filtTail, fs || cfg.FS, start, viewSamples);
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
