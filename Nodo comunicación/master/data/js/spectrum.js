// spectrum.js — per-channel FFT magnitude spectrum, plain canvas.
// Uses Hanning window + radix-2 Cooley-Tukey FFT, no external libraries.

import * as cfg from './config.js?v=field-study-19';

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
const SPEC_COLOR = cfg.RAW_COLOR;
const DB_FLOOR = -140;
const MIN_LOG_FREQ_HZ = 1e-6;

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

function trimFixed(value, digits) {
  const s = value.toFixed(digits);
  return s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s;
}

function formatFrequency(f, spaced = false) {
  const sep = spaced ? ' ' : '';
  if (!(f > 0)) return `0${sep}Hz`;
  if (f >= 1000) return `${trimFixed(f / 1000, f < 10000 ? 2 : 1)}${sep}kHz`;
  if (f >= 100) return `${trimFixed(f, 0)}${sep}Hz`;
  if (f >= 10) return `${trimFixed(f, 1)}${sep}Hz`;
  if (f >= 1) return `${trimFixed(f, 2)}${sep}Hz`;
  return `${trimFixed(f, 3)}${sep}Hz`;
}

function logFrequencyTicks(lo, hi, maxTicks = 8) {
  if (!(lo > 0) || !(hi > lo)) return [];
  const minExp = Math.floor(Math.log10(lo));
  const maxExp = Math.ceil(Math.log10(hi));
  const dense = [];
  for (let exp = minExp; exp <= maxExp; exp++) {
    for (const m of [1, 2, 5]) {
      const f = m * 10 ** exp;
      if (f >= lo * (1 - 1e-10) && f <= hi * (1 + 1e-10)) dense.push(f);
    }
  }
  if (dense.length <= maxTicks * 1.5) return dense;
  const decades = dense.filter((f) => {
    const exp = Math.round(Math.log10(f));
    return Math.abs(f - 10 ** exp) <= f * 1e-9;
  });
  return decades.length >= 2 ? decades : dense.filter((_, i) => i % Math.ceil(dense.length / maxTicks) === 0);
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

function normalizeSourceList(input, fallbackLabel = 'Cruda', fallbackColor = SPEC_COLOR) {
  if (!input) return [];
  if (ArrayBuffer.isView(input)) {
    return [{ label: fallbackLabel, color: fallbackColor, data: input }];
  }
  if (Array.isArray(input) && input.length && typeof input[0] === 'number') {
    return [{ label: fallbackLabel, color: fallbackColor, data: input }];
  }
  if (!Array.isArray(input)) return [];
  return input
    .map((source) => {
      if (!source) return null;
      const data = source.data || source.raw || source.values || null;
      if (!data || !data.length) return null;
      return {
        label: source.label || fallbackLabel,
        color: source.color || fallbackColor,
        data,
      };
    })
    .filter(Boolean);
}

function buildTrace(source, fs, isOverlay = false, overlayLabel = '') {
  const spec = computeSpectrum(source.data, fs);
  if (!spec) return null;
  const label = overlayLabel ? `${overlayLabel} ${source.label || ''}`.trim() : (source.label || '');
  return {
    label,
    color: source.color || SPEC_COLOR,
    spec,
    isOverlay,
  };
}

function minPositiveFrequencyFromSources(input, fs) {
  let minHz = Infinity;
  for (const source of normalizeSourceList(input)) {
    const len = source.data && source.data.length ? source.data.length : 0;
    if (len < 8) continue;
    const n = floorPow2(Math.min(len, 32768));
    if (n >= 8) minHz = Math.min(minHz, fs / n);
  }
  return Number.isFinite(minHz) ? minHz : Infinity;
}

function minPositiveFrequencyForView(nodeSources, overlayCaptures, fs) {
  let minHz = Infinity;
  for (const sources of nodeSources || []) {
    minHz = Math.min(minHz, minPositiveFrequencyFromSources(sources, fs));
  }
  for (const capture of overlayCaptures || []) {
    for (const node of capture.nodes || []) {
      if (!node) continue;
      const sources = node.spectrumSources || (node.raw ? [{ data: node.raw }] : null);
      minHz = Math.min(
        minHz,
        minPositiveFrequencyFromSources(sources, Number(node.fs || capture.fs || fs)),
      );
    }
  }
  if (Number.isFinite(minHz)) return Math.max(MIN_LOG_FREQ_HZ, minHz);
  return fs > 0 ? Math.max(MIN_LOG_FREQ_HZ, fs / 32768) : MIN_LOG_FREQ_HZ;
}

function logBounds(lo, hi, minFreq, maxFreq) {
  const minHz = Math.max(MIN_LOG_FREQ_HZ, minFreq || MIN_LOG_FREQ_HZ);
  const maxHz = Math.max(minHz * 1.000001, maxFreq || minHz * 10);
  let outLo = Number.isFinite(lo) && lo > 0 ? lo : minHz;
  let outHi = Number.isFinite(hi) && hi > 0 ? hi : maxHz;
  outLo = clamp(outLo, minHz, maxHz / 1.000001);
  outHi = clamp(outHi, outLo * 1.000001, maxHz);
  return {
    lo: outLo,
    hi: outHi,
    logLo: Math.log10(outLo),
    logHi: Math.log10(outHi),
  };
}

// ── SpectrumPlot ─────────────────────────────────────────────────────────────

class SpectrumPlot {
  constructor(container, title, handlers = {}) {
    this.title = title;
    this._primaryTraces = [];
    this._overlayTraces = [];
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

  setData(sources, fs, overlays = []) {
    this._primaryTraces = normalizeSourceList(sources)
      .map((source) => buildTrace(source, fs, false))
      .filter(Boolean);
    this._overlayTraces = [];
    for (const overlay of overlays || []) {
      const overlaySources = normalizeSourceList(
        overlay.sources || overlay.spectrumSources || overlay.raw,
        overlay.label || 'Preservada',
        overlay.color || SPEC_COLOR,
      );
      const overlayFs = Number(overlay.fs) || fs;
      for (const source of overlaySources) {
        const trace = buildTrace(
          { ...source, color: overlay.color || source.color },
          overlayFs,
          true,
          overlay.label || '',
        );
        if (trace) this._overlayTraces.push(trace);
      }
    }
    this._fs = fs || 0;
    for (const trace of [...this._primaryTraces, ...this._overlayTraces]) {
      const spec = trace.spec;
      if (spec && spec.df > 0 && spec.n > 0) {
        this._fs = Math.max(this._fs, spec.df * spec.n);
      }
    }
  }

  setFrequencyRange(lo, hi) {
    this._freqLo = Math.max(MIN_LOG_FREQ_HZ, Number.isFinite(lo) ? lo : MIN_LOG_FREQ_HZ);
    this._freqHi = Math.max(this._freqLo, Number.isFinite(hi) ? hi : this._freqLo);
  }

  clear() {
    this._primaryTraces = [];
    this._overlayTraces = [];
  }

  _minPositiveFrequency() {
    let minHz = Infinity;
    for (const item of [...this._primaryTraces, ...this._overlayTraces]) {
      const spec = item.spec;
      if (spec && spec.df > 0) minHz = Math.min(minHz, spec.df);
    }
    return Number.isFinite(minHz) ? Math.max(MIN_LOG_FREQ_HZ, minHz) : MIN_LOG_FREQ_HZ;
  }

  _logBounds() {
    if (!(this._fs > 0)) return null;
    return logBounds(this._freqLo, this._freqHi || this._fs / 2, this._minPositiveFrequency(), this._fs / 2);
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
      if ((!this._primaryTraces.length && !this._overlayTraces.length) || !this._fs) return;
      const rect = this.canvas.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const W = this.canvas.width;
      const H = this.canvas.height;
      const m = { left: MARGIN.left * dpr, right: MARGIN.right * dpr,
                   top: MARGIN.top * dpr,  bottom: MARGIN.bottom * dpr };
      const plotW = Math.max(1, W - m.left - m.right);
      const px = (ev.clientX - rect.left) * dpr;
      const bounds = this._logBounds();
      if (!bounds) return;
      const frac = clamp((px - m.left) / plotW, 0, 1);
      this._cursorHz = 10 ** (bounds.logLo + frac * (bounds.logHi - bounds.logLo));

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

    const drawableSpecs = [
      ...this._overlayTraces,
      ...this._primaryTraces,
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

    const bounds = this._logBounds();
    if (!bounds) { ctx.restore(); return; }
    const { lo: fLo, hi: fHi, logLo, logHi } = bounds;
    const logSpan = Math.max(1e-12, logHi - logLo);
    const xTo = (f) => m.left + ((Math.log10(Math.max(MIN_LOG_FREQ_HZ, f)) - logLo) / logSpan) * plotW;

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

    // X grid (Hz, logarithmic)
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    let lastLabelRight = -Infinity;
    for (const f of logFrequencyTicks(fLo, fHi, 8)) {
      if (f < fLo - 1e-9 || f > fHi + 1e-9) continue;
      const px = xTo(f);
      ctx.beginPath(); ctx.moveTo(px, m.top); ctx.lineTo(px, m.top + plotH); ctx.stroke();
      const lbl = formatFrequency(f);
      const textW = ctx.measureText(lbl).width;
      if (px - textW / 2 > lastLabelRight && px + textW / 2 < m.left + plotW + 2 * dpr) {
        ctx.fillText(lbl, px, m.top + plotH + 5 * dpr);
        lastLabelRight = px + textW / 2 + 6 * dpr;
      }
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
      ctx.strokeStyle = item.isOverlay ? withAlpha(item.color || SPEC_COLOR, 0.65) : (item.color || SPEC_COLOR);
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

      const hzLabel = formatFrequency(this._cursorHz, true);
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
    this._panLog = null;
    this._fMin = MIN_LOG_FREQ_HZ;
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

  _fullLogLo() {
    return Math.log10(Math.max(MIN_LOG_FREQ_HZ, this._fMin));
  }

  _fullLogHi() {
    return Math.log10(Math.max(this._fMin * 1.000001, this._fMax));
  }

  _viewLogSpan() {
    return Math.max(1e-12, (this._fullLogHi() - this._fullLogLo()) / Math.max(1, this._zoom));
  }

  _clampPan() {
    const fullLo = this._fullLogLo();
    const maxStart = Math.max(fullLo, this._fullLogHi() - this._viewLogSpan());
    if (!Number.isFinite(this._panLog)) this._panLog = fullLo;
    this._panLog = clamp(this._panLog, fullLo, maxStart);
  }

  _applyFrequencyWindow() {
    this._clampPan();
    const lo = 10 ** this._panLog;
    const hi = 10 ** (this._panLog + this._viewLogSpan());
    for (const p of this._plots) p.setFrequencyRange(lo, hi);
  }

  zoomBy(factor, anchorFrac = 0.5) {
    if (!(this._fMax > this._fMin)) return;
    const anchor = clamp(anchorFrac, 0, 1);
    const oldSpan = this._viewLogSpan();
    const oldLo = Number.isFinite(this._panLog) ? this._panLog : this._fullLogLo();
    this._zoom = clamp(this._zoom * factor, 1, 128);
    const newSpan = this._viewLogSpan();
    const anchorLog = oldLo + anchor * oldSpan;
    this._panLog = anchorLog - anchor * newSpan;
    this._applyFrequencyWindow();
  }

  panByFraction(frac) {
    if (!(this._fMax > this._fMin)) return;
    this._panLog = (Number.isFinite(this._panLog) ? this._panLog : this._fullLogLo()) + frac * this._viewLogSpan();
    this._applyFrequencyWindow();
  }

  resetView() {
    this._zoom = 1;
    this._panLog = this._fullLogLo();
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

  update(nodeSources, fs, overlayCaptures = []) {
    this._fMax = fs > 0 ? fs / 2 : 0;
    for (const capture of overlayCaptures || []) {
      const captureFs = Number(capture.fs) || 0;
      if (captureFs > 0) this._fMax = Math.max(this._fMax, captureFs / 2);
      for (const node of capture.nodes || []) {
        const nodeFs = Number(node && node.fs) || 0;
        if (nodeFs > 0) this._fMax = Math.max(this._fMax, nodeFs / 2);
      }
    }
    this._fMin = this._fMax > 0 ? minPositiveFrequencyForView(nodeSources, overlayCaptures, fs) : MIN_LOG_FREQ_HZ;
    if (this._zoom === 1 || !Number.isFinite(this._panLog)) this._panLog = this._fullLogLo();
    this._applyFrequencyWindow();
    for (let i = 0; i < this._plots.length; i++) {
      const p = this._plots[i];
      if (!p._visible) continue;
      const overlays = [];
      for (const capture of overlayCaptures || []) {
        const node = capture.nodes && capture.nodes[i];
        if (!node) continue;
        const sources = node.spectrumSources || (node.raw ? [{ label: 'Cruda', color: capture.color, data: node.raw }] : null);
        if (!sources || !sources.length) continue;
        overlays.push({ label: capture.label, color: capture.color, sources, fs: Number(node.fs || capture.fs || fs) });
      }
      p.setData(nodeSources[i], fs, overlays);
      p.draw();
    }
  }

  clearAll() {
    for (const p of this._plots) { p.clear(); p.draw(); }
  }
}
