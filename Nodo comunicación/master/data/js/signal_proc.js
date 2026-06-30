// signal_proc.js - FIR presets and DC removal helpers.
// Mirrors python/geophone_scope/signal_proc.py for the web UI subset:
// lp/hp/bp/bs FIR presets only, streaming FIR state, full-buffer DC removal.

let firCompileError = '';

export function lastFirError() {
  return firCompileError;
}

function setFirError(message) {
  firCompileError = message || '';
}

function sinc(x) {
  if (Math.abs(x) < 1e-12) return 1.0;
  const pix = Math.PI * x;
  return Math.sin(pix) / pix;
}

function validateCutoff(freq, fs, name) {
  if (!Number.isFinite(freq)) throw new Error(`${name} must be numeric`);
  if (freq <= 0 || freq >= fs / 2) throw new Error(`${name} must be between 0 and Nyquist`);
}

function hamming(n, nTaps) {
  if (nTaps <= 1) return 1.0;
  return 0.54 - 0.46 * Math.cos((2 * Math.PI * n) / (nTaps - 1));
}

function firwin(numTaps, bands, scaleFrequency) {
  const alpha = 0.5 * (numTaps - 1);
  const b = new Float64Array(numTaps);
  for (let n = 0; n < numTaps; n++) {
    const m = n - alpha;
    let h = 0;
    for (const [left, right] of bands) {
      h += right * sinc(right * m) - left * sinc(left * m);
    }
    b[n] = h * hamming(n, numTaps);
  }

  let response = 0;
  for (let n = 0; n < numTaps; n++) {
    const m = n - alpha;
    response += b[n] * Math.cos(Math.PI * m * scaleFrequency);
  }
  if (Math.abs(response) < 1e-15) throw new Error('cannot scale FIR coefficients');
  for (let n = 0; n < numTaps; n++) b[n] /= response;
  return b;
}

function lowpass(numTaps, cutoff, fs) {
  validateCutoff(cutoff, fs, 'cutoff');
  const c = cutoff / (fs / 2);
  return firwin(numTaps, [[0, c]], 0);
}

function highpass(numTaps, cutoff, fs) {
  validateCutoff(cutoff, fs, 'cutoff');
  const c = cutoff / (fs / 2);
  return firwin(numTaps, [[c, 1]], 1);
}

function bandpass(numTaps, low, high, fs) {
  validateCutoff(low, fs, 'low cutoff');
  validateCutoff(high, fs, 'high cutoff');
  if (low >= high) throw new Error('low cutoff must be below high cutoff');
  const lo = low / (fs / 2);
  const hi = high / (fs / 2);
  return firwin(numTaps, [[lo, hi]], 0.5 * (lo + hi));
}

function bandstop(numTaps, low, high, fs) {
  validateCutoff(low, fs, 'low cutoff');
  validateCutoff(high, fs, 'high cutoff');
  if (low >= high) throw new Error('low cutoff must be below high cutoff');
  const lo = low / (fs / 2);
  const hi = high / (fs / 2);
  return firwin(numTaps, [[0, lo], [hi, 1]], 0);
}

function ensureOddTapCount(nTaps) {
  if (!Number.isFinite(nTaps)) throw new Error('numtaps must be numeric');
  nTaps = Math.round(nTaps);
  if (nTaps < 3) throw new Error('numtaps must be >= 3');
  // scipy.firwin high-pass / band-stop with passband to Nyquist require odd taps.
  return (nTaps % 2 === 0) ? nTaps + 1 : nTaps;
}

function parseParts(cmd) {
  const parts = cmd.trim().toLowerCase().split(/\s+/).filter(Boolean);
  if (!parts.length) throw new Error('empty command');
  let nTaps = 101;
  if (parts[0] === 'numtaps') {
    if (parts.length < 3) throw new Error('numtaps needs <n> and a filter type');
    nTaps = ensureOddTapCount(Number(parts[1]));
    parts.splice(0, 2);
  }
  return { nTaps: ensureOddTapCount(nTaps), parts };
}

/**
 * Compile a preset FIR command into coefficients.
 * Supported:
 *   lp <cutoff>
 *   hp <cutoff>
 *   bp <low> <high>
 *   bs/sb/bandstop/stopband <low> <high>
 *   bs/sb/bandstop/stopband <center>  (center +/- 5 Hz)
 *   numtaps <n> ...       (explicit tap count; even values are rounded up)
 */
export function compileFirCmd(cmd, fs) {
  setFirError('');
  try {
    if (!Number.isFinite(fs) || fs <= 0) throw new Error('fs must be positive');
    const { nTaps, parts } = parseParts(cmd);
    const ftype = parts[0];
    let b;
    if (ftype === 'lp' || ftype === 'lowpass') {
      if (parts.length < 2) throw new Error('lp needs <cutoff>');
      b = lowpass(nTaps, Number(parts[1]), fs);
    } else if (ftype === 'hp' || ftype === 'highpass') {
      if (parts.length < 2) throw new Error('hp needs <cutoff>');
      b = highpass(nTaps, Number(parts[1]), fs);
    } else if (ftype === 'bp' || ftype === 'bandpass') {
      if (parts.length < 3) throw new Error('bp needs <low> <high>');
      b = bandpass(nTaps, Number(parts[1]), Number(parts[2]), fs);
    } else if (['bs', 'sb', 'bandstop', 'stopband'].includes(ftype)) {
      if (parts.length < 2) throw new Error('bs/sb needs <center> or <low> <high>');
      let low;
      let high;
      if (parts.length === 2) {
        const center = Number(parts[1]);
        low = center - 5.0;
        high = center + 5.0;
      } else {
        low = Number(parts[1]);
        high = Number(parts[2]);
      }
      b = bandstop(nTaps, low, high, fs);
    } else {
      throw new Error(`unsupported web FIR preset: ${ftype}`);
    }
    if (!b || b.length === 0 || ![...b].every(Number.isFinite)) {
      throw new Error('invalid coefficients');
    }
    return b;
  } catch (err) {
    setFirError(err && err.message ? err.message : String(err));
    return null;
  }
}

/**
 * Streaming FIR equivalent to scipy.signal.lfilter(b, 1, x, zi=...).
 * zi is the direct-form II transposed state (length b.length - 1).
 */
export function firFilter(b, x, zi = null) {
  const coeff = ArrayBuffer.isView(b) ? b : new Float64Array(b);
  const input = ArrayBuffer.isView(x) ? x : new Float64Array(x);
  const state = zi ? new Float64Array(zi) : new Float64Array(Math.max(0, coeff.length - 1));
  const y = new Float64Array(input.length);

  if (coeff.length === 0) return { y, zi: state };
  if (coeff.length === 1) {
    for (let n = 0; n < input.length; n++) y[n] = coeff[0] * input[n];
    return { y, zi: state };
  }

  for (let n = 0; n < input.length; n++) {
    const xn = input[n];
    const yn = coeff[0] * xn + state[0];
    y[n] = yn;
    for (let i = 0; i < state.length - 1; i++) {
      state[i] = coeff[i + 1] * xn + state[i + 1];
    }
    state[state.length - 1] = coeff[coeff.length - 1] * xn;
  }
  return { y, zi: state };
}

function reverseCopy(input) {
  const out = new Float64Array(input.length);
  for (let i = 0; i < input.length; i++) out[i] = input[input.length - 1 - i];
  return out;
}

/**
 * Zero-phase FIR filtering, equivalent in spirit to scipy.signal.filtfilt(b, 1, x).
 * Uses odd reflection padding so edge transients fall mostly outside the returned
 * capture. This is for full-buffer display/export, not real-time hardware FIR.
 */
export function filtFilt(b, x) {
  const coeff = ArrayBuffer.isView(b) ? b : new Float64Array(b);
  const input = ArrayBuffer.isView(x) ? x : new Float64Array(x);
  const n = input.length;
  if (!n) return new Float64Array(0);
  if (coeff.length === 0) return new Float64Array(input);
  if (coeff.length === 1) {
    const gain = coeff[0] * coeff[0];
    const out = new Float64Array(n);
    for (let i = 0; i < n; i++) out[i] = input[i] * gain;
    return out;
  }

  const padLen = Math.min(n - 1, Math.max(0, 3 * (coeff.length - 1)));
  const ext = new Float64Array(n + 2 * padLen);
  for (let i = 0; i < padLen; i++) {
    ext[i] = 2 * input[0] - input[padLen - i];
  }
  ext.set(input, padLen);
  for (let i = 0; i < padLen; i++) {
    ext[padLen + n + i] = 2 * input[n - 1] - input[n - 2 - i];
  }

  const forward = firFilter(coeff, ext, null).y;
  const backward = firFilter(coeff, reverseCopy(forward), null).y;
  const zeroPhase = reverseCopy(backward);
  return zeroPhase.slice(padLen, padLen + n);
}

export function dcRemove(buf) {
  const x = ArrayBuffer.isView(buf) ? buf : new Float64Array(buf);
  if (!x.length) return new Float64Array(0);
  let sum = 0;
  for (let i = 0; i < x.length; i++) sum += x[i];
  const mean = sum / x.length;
  const out = new Float64Array(x.length);
  for (let i = 0; i < x.length; i++) out[i] = x[i] - mean;
  return out;
}

function nextPow2(n) {
  let p = 1;
  while (p < n) p <<= 1;
  return p;
}

function fftRadix2(re, im, inverse = false) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      const tr = re[i]; re[i] = re[j]; re[j] = tr;
      const ti = im[i]; im[i] = im[j]; im[j] = ti;
    }
  }

  for (let len = 2; len <= n; len <<= 1) {
    const ang = (inverse ? 2 : -2) * Math.PI / len;
    const wlenR = Math.cos(ang);
    const wlenI = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let wr = 1;
      let wi = 0;
      for (let j = 0; j < len / 2; j++) {
        const uR = re[i + j];
        const uI = im[i + j];
        const vR = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
        const vI = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
        re[i + j] = uR + vR;
        im[i + j] = uI + vI;
        re[i + j + len / 2] = uR - vR;
        im[i + j + len / 2] = uI - vI;
        const nextWr = wr * wlenR - wi * wlenI;
        wi = wr * wlenI + wi * wlenR;
        wr = nextWr;
      }
    }
  }

  if (inverse) {
    for (let i = 0; i < n; i++) {
      re[i] /= n;
      im[i] /= n;
    }
  }
}

export function hilbertEnvelope(buf) {
  const input = ArrayBuffer.isView(buf) ? buf : new Float64Array(buf);
  const nInput = input.length;
  if (!nInput) return new Float64Array(0);
  const n = nextPow2(nInput);
  const re = new Float64Array(n);
  const im = new Float64Array(n);
  re.set(input);

  fftRadix2(re, im, false);
  if (n > 1) {
    const half = n >> 1;
    for (let k = 1; k < half; k++) {
      re[k] *= 2;
      im[k] *= 2;
    }
    for (let k = half + 1; k < n; k++) {
      re[k] = 0;
      im[k] = 0;
    }
  }
  fftRadix2(re, im, true);

  const out = new Float64Array(nInput);
  for (let i = 0; i < nInput; i++) out[i] = Math.hypot(re[i], im[i]);
  return out;
}
