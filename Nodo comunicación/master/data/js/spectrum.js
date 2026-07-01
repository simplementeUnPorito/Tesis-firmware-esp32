// spectrum.js — per-channel FFT magnitude spectrum, plain canvas.
// Uses Hanning window + radix-2 Cooley-Tukey FFT, no external libraries.

import * as cfg from './config.js?v=field-study-10';

// ── FFT ──────────────────────────────────────────────────────────────────────

function floorPow2(n) {
  let p = 1;
  while ((p << 1) <= n) p <<= 1;
  return p;
}

function hanningWindow(n) {
  const w = new Float64Array(n);
  for (let i = 0; i < n; i++) w[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / (n - 1)));
  return w;
}

function fftInPlace(re, im) {
  const n = re.length;
  let j = 0;
  for (let i = 1; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const half = len >> 1;
    const ang = -2 * Math.PI / len;
    const wRe = Math.cos(ang), wIm = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let curRe = 1, curIm = 0;
      for (let k = 0; k < half; k++) {
        const uRe = re[i+k], uIm = im[i+k];
        const vRe = re[i+k+half] * curRe - im[i+k+half] * curIm;
        const vIm = re[i+k+half] * curIm + im[i+k+half] * curRe;
        re[i+k] = uRe + vRe;  im[i+k] = uIm + vIm;
        re[i+k+half] = uRe - vRe; im[i+k+half] = uIm - vIm;
        const nr = curRe * wRe - curIm * wIm;
        curIm = curRe * wIm + curIm * wRe;
        curRe = nr;
      }
    }
  }
}

export function computeSpectrum(data, fs) {
  if (!data || data.length < 8 || !(fs > 0)) return null;
  const nRaw = Math.min(data.length, 32768);
  const n = floorPow2(nRaw);
  const win = hanningWindow(n);
  const re = new Float64Array(n);
  const im = new Float64Array(n);
  const src = data.subarray(data.length - n);
  let mean = 0;
  let winSum = 0;
  for (let i = 0; i < n; i++) mean += src[i];
  mean /= n;
  for (let i = 0; i < n; i++) {
    re[i] = (src[i] - mean) * win[i];
    winSum += win[i];
  }
  fftInPlace(re, im);
  const half = (n >> 1) + 1;
  const mag = new Float64Array(half);
  const freqs = new Float64Array(half);
  const df = fs / n;
  const scale = winSum > 0 ? 1 / winSum : 1 / n;
  for (let k = 0; k < half; k++) {
    const singleSide = (k === 0 || k === half - 1) ? 1 : 2;
    mag[k] = singleSide * Math.sqrt(re[k] * re[k] + im[k] * im[k]) * scale;
    freqs[k] = k * df;
  }
  return { mag, freqs, df, n };
}

// ── Helpers (mirrored from plot.js so spectrum.js stays self-contained) ──────

const MARGIN = { left: 46, right: 34, top: 12, bottom: 20 };
const SPEC_COLOR = 'rgb(80,210,160)';
const DB_FLOOR = -140;

function niceNum(range, round) {
  if (!(range > 0)) return 1;
  const exp = Math.floor(Math.log10(range));
  const frac = range / 10 ** exp;
  let niceFrac;
  if (round) niceFrac = frac < 1.5 ? 1 : frac < 3 ? 2 : frac < 7 ? 5 : 10;
  else niceFrac = frac <= 1 ? 1 : frac <= 2 ? 2 : frac <= 5 ? 5 : 10;
  return niceFrac * 10 ** exp;
}

function niceTicks(lo, hi, count) {
  if (lo === hi) { lo -= 0.5; hi += 0.5; }
  const step = niceNum(niceNum(hi - lo, false) / Math.max(1, count - 1), true);
  const niceLo = Math.floor(lo / step) * step;
  const niceHi = Math.ceil(hi / step) * step;
  const ticks = [];
  for (let v = niceLo; v <= niceHi + step / 2; v += step) ticks.push(v);
  return ticks;
}

function cssVar(el, name, fallback) {
  const v = getComputedStyle(el).getPropertyValue(name).trim();
  return v || fallback;
}

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

function withAlpha(color, alpha) {
  const a = clamp(alpha, 0, 1);
  const rgba = color.match(/^rgba?\(([^,]+),\s*([^,]+),\s*([^,\)]+)(?:,\s*([^\)]+))?\)$/i);
  if (!rgba) return color;
  return `rgba(${rgba[1]}, ${rgba[2]}, ${rgba[3]}, ${a})`;
}

// ── SpectrumPlot ─────────────────────────────────────────────────────────────

class SpectrumPlot {
  constructor(container, title, handlers = {}) {
    this.title = title;
    this._spec = null;
    this._overlays = [];
    this._fs = 0;
    this._visible = true;
    this._cursorHz = null;   // cursor position in Hz (null = no cursor)
    this._freqLo = 0;
    this._freqHi = 0;
    this._handlers = handlers;
    this._dragLastX = null;
    this._dragMoved = false;

    this.wrap = document.createElement('div');
    this.wrap.className = 'plot-wrap';
    this.canvas = document.createElement('canvas');
    this.wrap.appendChild(this.canvas);
    container.appendChild(this.wrap);
    this._ctx = this.canvas.getContext('2d');
    this._ro = new ResizeObserver(() => this._fitCanvas());
    this._ro.observe(this.wrap);
    this._wirePointer();
    this._fitCanvas();
  }

  setVisible(v) {
    this._visible = !!v;
    this.wrap.style.display = v ? '' : 'none';
    if (v) this._fitCanvas();
  }

  setTitle(title) {
    this.title = title || this.title;
  }

  setData(data, fs, overlays = []) {
    this._spec = computeSpectrum(data, fs);
    this._overlays = (overlays || [])
      .map((overlay) => ({
        label: overlay.label,
        color: overlay.color,
        spec: computeSpectrum(overlay.raw, fs),
      }))
      .filter((overlay) => !!overlay.spec);
    this._fs = fs || 0;
  }

  setFrequencyRange(lo, hi) {
    this._freqLo = Math.max(0, Number.isFinite(lo) ? lo : 0);
    this._freqHi = Math.max(this._freqLo, Number.isFinite(hi) ? hi : 0);
  }

  clear() {
    this._spec = null;
    this._overlays = [];
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

  _wirePointer() {
    this.canvas.style.cursor = 'crosshair';
    this.canvas.style.touchAction = 'none';
    this.canvas.addEventListener('wheel', (ev) => {
      if (!this._handlers.onZoom) return;
      ev.preventDefault();
      const rect = this.canvas.getBoundingClientRect();
      const frac = clamp((ev.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
      this._handlers.onZoom(ev.deltaY < 0 ? 1.25 : 1 / 1.25, frac);
    }, { passive: false });
    this.canvas.addEventListener('pointerdown', (ev) => {
      this.canvas.setPointerCapture(ev.pointerId);
      this._dragLastX = ev.clientX;
      this._downX = ev.clientX;
      this._downY = ev.clientY;
      this._downButton = ev.button;
      this._dragMoved = false;
    });
    this.canvas.addEventListener('pointermove', (ev) => {
      if (!this._spec || !this._fs) return;
      const rect = this.canvas.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const W = this.canvas.width;
      const H = this.canvas.height;
      const m = { left: MARGIN.left * dpr, right: MARGIN.right * dpr,
                   top: MARGIN.top * dpr,  bottom: MARGIN.bottom * dpr };
      const plotW = Math.max(1, W - m.left - m.right);
      const px = (ev.clientX - rect.left) * dpr;
      const fMax = this._fs / 2;
      const fLo = clamp(this._freqLo, 0, fMax);
      const fHi = clamp(this._freqHi || fMax, fLo + 1e-9, fMax);
      this._cursorHz = clamp(fLo + ((px - m.left) / plotW) * (fHi - fLo), fLo, fHi);

      if (this._dragLastX !== null) {
        if (!this._dragMoved
            && (Math.abs(ev.clientX - this._downX) > 4
                || Math.abs(ev.clientY - this._downY) > 4)) {
          this._dragMoved = true;
        }
        if (this._dragMoved && this._handlers.onPan) {
          const dx = ev.clientX - this._dragLastX;
          this._handlers.onPan(-dx / Math.max(1, rect.width));
        }
        this._dragLastX = ev.clientX;
      }
    });
    const endPointer = (ev) => {
      if (this.canvas.hasPointerCapture(ev.pointerId)) {
        this.canvas.releasePointerCapture(ev.pointerId);
      }
      if (this._dragLastX !== null && !this._dragMoved && this._handlers.onZoom) {
        const rect = this.canvas.getBoundingClientRect();
        const frac = clamp((ev.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
        const zoomOut = this._downButton === 2 || ev.shiftKey;
        this._handlers.onZoom(zoomOut ? 1 / 1.5 : 1.5, frac);
      }
      this._dragLastX = null;
      this._dragMoved = false;
    };
    this.canvas.addEventListener('pointerup', endPointer);
    this.canvas.addEventListener('pointercancel', endPointer);
    this.canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
    this.canvas.addEventListener('pointerleave', () => {
      this._cursorHz = null;
      this._dragLastX = null;
    });
  }

  draw() {
    if (!this._visible) return;
    const ctx = this._ctx;
    const dpr = window.devicePixelRatio || 1;
    const W = this.canvas.width, H = this.canvas.height;
    const m = {
      left: MARGIN.left * dpr, right: MARGIN.right * dpr,
      top: MARGIN.top * dpr,   bottom: MARGIN.bottom * dpr,
    };
    const plotW = Math.max(1, W - m.left - m.right);
    const plotH = Math.max(1, H - m.top - m.bottom);
    const colors = {
      bg:     cssVar(document.body, '--plot-bg',     '#000'),
      grid:   cssVar(document.body, '--plot-grid',   'rgba(255,255,255,0.12)'),
      border: cssVar(document.body, '--plot-border', 'rgba(255,255,255,0.25)'),
      axis:   cssVar(document.body, '--plot-axis',   '#9ab'),
      title:  cssVar(document.body, '--plot-title',  '#ddd'),
    };

    ctx.save();
    ctx.fillStyle = colors.bg;
    ctx.fillRect(0, 0, W, H);

    const spec = this._spec;
    const overlaySpecs = this._overlays || [];
    const drawableSpecs = [
      ...overlaySpecs.map((overlay) => ({ ...overlay, isOverlay: true })),
      ...(spec ? [{ spec, color: SPEC_COLOR, isOverlay: false }] : []),
    ];
    if (!drawableSpecs.length || !(this._fs > 0)) {
      ctx.font = `${Math.round(11 * dpr)}px system-ui`;
      ctx.fillStyle = colors.axis;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('sin datos', W / 2, H / 2);
      ctx.restore();
      return;
    }

    const fMax = this._fs / 2;
    const fLo = clamp(this._freqLo, 0, fMax);
    const fHi = clamp(this._freqHi || fMax, fLo + 1e-9, fMax);
    const fSpan = Math.max(1e-9, fHi - fLo);
    const xTo = (f) => m.left + ((f - fLo) / fSpan) * plotW;

    const specsWithDb = drawableSpecs.map((item) => {
      const { mag } = item.spec;
      const db = new Float64Array(mag.length);
      let dbMax = -Infinity, dbMin = Infinity;
      for (let i = 1; i < mag.length; i++) {
        db[i] = Math.max(DB_FLOOR, 20 * Math.log10(Math.max(mag[i], 1e-15)));
        if (db[i] > dbMax) dbMax = db[i];
        if (db[i] < dbMin) dbMin = db[i];
      }
      return { ...item, db, dbMax, dbMin };
    }).filter((item) => Number.isFinite(item.dbMax) && Number.isFinite(item.dbMin));
    if (!specsWithDb.length) { ctx.restore(); return; }
    let dbMax = -Infinity, dbMin = Infinity;
    for (const item of specsWithDb) {
      dbMax = Math.max(dbMax, item.dbMax);
      dbMin = Math.min(dbMin, item.dbMin);
    }
    let yHi = Math.ceil((dbMax + 3) / 5) * 5;
    let yLo = Math.floor((dbMin - 3) / 5) * 5;
    if (yHi - yLo < 20) {
      const mid = (yHi + yLo) / 2;
      yHi = mid + 10;
      yLo = mid - 10;
    }
    const yTo = (v) => m.top + plotH - ((v - yLo) / Math.max(1e-9, yHi - yLo)) * plotH;

    ctx.lineWidth = Math.max(1, dpr);
    ctx.font = `${Math.round(10 * dpr)}px ui-monospace, monospace`;

    // Y grid (dB)
    ctx.strokeStyle = colors.grid;
    ctx.fillStyle = colors.axis;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const v of niceTicks(yLo, yHi, 5)) {
      const py = yTo(v);
      if (py < m.top - 1 || py > m.top + plotH + 1) continue;
      ctx.beginPath(); ctx.moveTo(m.left, py); ctx.lineTo(m.left + plotW, py); ctx.stroke();
      ctx.fillText(`${v.toFixed(0)}`, m.left - 6 * dpr, py);
    }

    // X grid (Hz)
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (const f of niceTicks(fLo, fHi, 6)) {
      if (f < fLo - 1e-9 || f > fHi + 1e-9) continue;
      const px = xTo(f);
      ctx.beginPath(); ctx.moveTo(px, m.top); ctx.lineTo(px, m.top + plotH); ctx.stroke();
      const lbl = f >= 1000 ? `${(f / 1000).toFixed(1)}k` : `${f.toFixed(0)}`;
      ctx.fillText(`${lbl}Hz`, px, m.top + plotH + 5 * dpr);
    }

    ctx.strokeStyle = colors.border;
    ctx.strokeRect(m.left, m.top, plotW, plotH);

    // Spectrum curves
    ctx.save();
    ctx.beginPath();
    ctx.rect(m.left, m.top, plotW, plotH);
    ctx.clip();
    ctx.lineWidth = Math.max(1, dpr);
    for (const item of specsWithDb) {
      const { freqs } = item.spec;
      ctx.strokeStyle = item.isOverlay ? withAlpha(item.color || SPEC_COLOR, 0.65) : SPEC_COLOR;
      ctx.setLineDash(item.isOverlay ? [5 * dpr, 3 * dpr] : []);
      ctx.beginPath();
      let started = false;
      for (let i = 1; i < freqs.length; i++) {
        const px = xTo(freqs[i]);
        const py = yTo(item.db[i]);
        if (!started) { ctx.moveTo(px, py); started = true; }
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
    }
    ctx.setLineDash([]);
    ctx.restore();

    // Hover cursor
    if (this._cursorHz !== null) {
      const cpx = xTo(this._cursorHz);
      // Find nearest bin
      const cursorSpec = specsWithDb.find((item) => !item.isOverlay) || specsWithDb[0];
      const binIdx = Math.max(1, Math.round(this._cursorHz / (cursorSpec.spec.df || 1)));
      const clampedIdx = clamp(binIdx, 1, cursorSpec.db.length - 1);
      const ampDb = cursorSpec.db[clampedIdx];

      ctx.save();
      ctx.strokeStyle = 'rgba(255,230,80,0.8)';
      ctx.lineWidth = dpr;
      ctx.setLineDash([4 * dpr, 3 * dpr]);
      ctx.beginPath(); ctx.moveTo(cpx, m.top); ctx.lineTo(cpx, m.top + plotH); ctx.stroke();
      ctx.setLineDash([]);

      const hzLabel = this._cursorHz >= 1000 ? `${(this._cursorHz / 1000).toFixed(2)} kHz` : `${this._cursorHz.toFixed(1)} Hz`;
      const lines = [hzLabel, `${ampDb.toFixed(1)} dB`];
      ctx.font = `${Math.round(10 * dpr)}px ui-monospace, monospace`;
      const pad = 5 * dpr, lineH = 13 * dpr;
      const boxW = Math.max(...lines.map((l) => ctx.measureText(l).width)) + pad * 2 + 4 * dpr;
      const boxH = lines.length * lineH + pad * 2;
      let bx = cpx + 8 * dpr;
      if (bx + boxW > m.left + plotW - 2 * dpr) bx = cpx - boxW - 8 * dpr;
      const by = m.top + pad;

      ctx.fillStyle = 'rgba(20,20,20,0.85)';
      ctx.strokeStyle = 'rgba(255,230,80,0.6)';
      ctx.lineWidth = dpr;
      ctx.beginPath();
      if (ctx.roundRect) ctx.roundRect(bx, by, boxW, boxH, 3 * dpr);
      else ctx.rect(bx, by, boxW, boxH);
      ctx.fill(); ctx.stroke();

      ctx.fillStyle = '#ffe980';
      ctx.textAlign = 'left';
      ctx.textBaseline = 'top';
      for (let li = 0; li < lines.length; li++)
        ctx.fillText(lines[li], bx + pad, by + pad + li * lineH);
      ctx.restore();
    }

    // Title + Y label
    ctx.font = `${Math.round(11 * dpr)}px system-ui, sans-serif`;
    ctx.fillStyle = colors.title;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    ctx.fillText(this.title, m.left, 2 * dpr);

    ctx.save();
    ctx.font = `${Math.round(9 * dpr)}px ui-monospace, monospace`;
    ctx.fillStyle = colors.axis;
    ctx.translate(9 * dpr, m.top + plotH / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('dB', 0, 0);
    ctx.restore();

    ctx.restore();
  }
}

// ── SpectrumArea ──────────────────────────────────────────────────────────────

export class SpectrumArea {
  constructor(container) {
    this._container = container;
    this._plots = [];
    this._zoom = 1;
    this._panHz = 0;
    this._fMax = 0;
    const handlers = {
      onZoom: (factor, anchor) => this.zoomBy(factor, anchor),
      onPan: (frac) => this.panByFraction(frac),
    };
    for (let i = 0; i < cfg.MAX_NODES; i++) {
      this._plots.push(new SpectrumPlot(container, cfg.NODE_NAMES[i], handlers));
    }
    for (const p of this._plots) p.setVisible(false);
  }

  _viewHz() {
    return Math.max(1, this._fMax / Math.max(1, this._zoom));
  }

  _clampPan() {
    const maxStart = Math.max(0, this._fMax - this._viewHz());
    this._panHz = clamp(this._panHz, 0, maxStart);
  }

  _applyFrequencyWindow() {
    this._clampPan();
    const lo = this._panHz;
    const hi = lo + this._viewHz();
    for (const p of this._plots) p.setFrequencyRange(lo, hi);
  }

  zoomBy(factor, anchorFrac = 0.5) {
    if (!(this._fMax > 0)) return;
    const anchor = clamp(anchorFrac, 0, 1);
    const oldSpan = this._viewHz();
    const oldLo = this._panHz;
    this._zoom = clamp(this._zoom * factor, 1, 128);
    const newSpan = this._viewHz();
    const anchorHz = oldLo + anchor * oldSpan;
    this._panHz = anchorHz - anchor * newSpan;
    this._applyFrequencyWindow();
  }

  panByFraction(frac) {
    if (!(this._fMax > 0)) return;
    this._panHz += frac * this._viewHz();
    this._applyFrequencyWindow();
  }

  resetView() {
    this._zoom = 1;
    this._panHz = 0;
    this._applyFrequencyWindow();
  }

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

  setNodeTitles(titles) {
    for (let i = 0; i < this._plots.length; i++) {
      if (titles && titles[i]) this._plots[i].setTitle(titles[i]);
    }
  }

  update(rawBufs, fs, overlayCaptures = []) {
    this._fMax = fs > 0 ? fs / 2 : 0;
    this._applyFrequencyWindow();
    for (let i = 0; i < this._plots.length; i++) {
      const p = this._plots[i];
      if (!p._visible) continue;
      const overlays = [];
      for (const capture of overlayCaptures || []) {
        const node = capture.nodes && capture.nodes[i];
        if (!node || !node.raw || !node.raw.length) continue;
        overlays.push({ label: capture.label, color: capture.color, raw: node.raw });
      }
      p.setData(rawBufs[i], fs, overlays);
      p.draw();
    }
  }

  clearAll() {
    for (const p of this._plots) { p.clear(); p.draw(); }
  }
}
