// spectrum.js — per-channel FFT magnitude spectrum, plain canvas.
// Uses Hanning window + radix-2 Cooley-Tukey FFT, no external libraries.

import * as cfg from './config.js?v=field-loop-19';

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

const MARGIN = { left: 46, right: 10, top: 12, bottom: 20 };
const SPEC_COLOR = 'rgb(80,210,160)';

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

// ── SpectrumPlot ─────────────────────────────────────────────────────────────

class SpectrumPlot {
  constructor(container, title) {
    this.title = title;
    this._spec = null;
    this._fs = 0;
    this._visible = true;
    this._cursorHz = null;   // cursor position in Hz (null = no cursor)

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

  setData(data, fs) {
    this._spec = computeSpectrum(data, fs);
    this._fs = fs || 0;
  }

  clear() { this._spec = null; }

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
      this._cursorHz = clamp((px - m.left) / plotW * fMax, 0, fMax);
    });
    this.canvas.addEventListener('pointerleave', () => { this._cursorHz = null; });
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
    if (!spec || !(this._fs > 0)) {
      ctx.font = `${Math.round(11 * dpr)}px system-ui`;
      ctx.fillStyle = colors.axis;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('sin datos', W / 2, H / 2);
      ctx.restore();
      return;
    }

    const { mag, freqs } = spec;
    const fMax = this._fs / 2;
    const xTo = (f) => m.left + (f / Math.max(1e-9, fMax)) * plotW;

    // Build dB array
    const db = new Float64Array(mag.length);
    let dbMax = -Infinity, dbMin = Infinity;
    for (let i = 1; i < mag.length; i++) {
      db[i] = 20 * Math.log10(Math.max(mag[i], 1e-15));
      if (db[i] > dbMax) dbMax = db[i];
      if (mag[i] > 1e-12 && db[i] < dbMin) dbMin = db[i];
    }
    if (!Number.isFinite(dbMax)) { ctx.restore(); return; }
    const dispRange = Math.max(20, Math.min(60, dbMax - dbMin));
    const yHi = dbMax + 3;
    const yLo = yHi - dispRange;
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
    for (const f of niceTicks(0, fMax, 6)) {
      if (f < -1e-9 || f > fMax + 1e-9) continue;
      const px = xTo(f);
      ctx.beginPath(); ctx.moveTo(px, m.top); ctx.lineTo(px, m.top + plotH); ctx.stroke();
      const lbl = f >= 1000 ? `${(f / 1000).toFixed(1)}k` : `${f.toFixed(0)}`;
      ctx.fillText(`${lbl}Hz`, px, m.top + plotH + 5 * dpr);
    }

    ctx.strokeStyle = colors.border;
    ctx.strokeRect(m.left, m.top, plotW, plotH);

    // Spectrum curve
    ctx.save();
    ctx.beginPath();
    ctx.rect(m.left, m.top, plotW, plotH);
    ctx.clip();
    ctx.strokeStyle = SPEC_COLOR;
    ctx.lineWidth = Math.max(1, dpr);
    ctx.beginPath();
    let started = false;
    for (let i = 1; i < mag.length; i++) {
      const px = xTo(freqs[i]);
      const py = yTo(db[i]);
      if (!started) { ctx.moveTo(px, py); started = true; }
      else ctx.lineTo(px, py);
    }
    ctx.stroke();
    ctx.restore();

    // Hover cursor
    if (this._cursorHz !== null) {
      const cpx = xTo(this._cursorHz);
      // Find nearest bin
      const binIdx = Math.max(1, Math.round(this._cursorHz / (spec.df || 1)));
      const clampedIdx = clamp(binIdx, 1, mag.length - 1);
      const ampDb = db[clampedIdx];

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
    for (let i = 0; i < cfg.MAX_NODES; i++) {
      this._plots.push(new SpectrumPlot(container, cfg.NODE_NAMES[i]));
    }
    for (const p of this._plots) p.setVisible(false);
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

  update(rawBufs, fs) {
    for (let i = 0; i < this._plots.length; i++) {
      const p = this._plots[i];
      if (!p._visible) continue;
      p.setData(rawBufs[i], fs);
      p.draw();
    }
  }

  clearAll() {
    for (const p of this._plots) { p.clear(); p.draw(); }
  }
}
