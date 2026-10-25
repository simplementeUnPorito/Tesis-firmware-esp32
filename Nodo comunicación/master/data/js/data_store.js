// data_store.js — per-node circular sample buffers and acquisition stats.
// Mirrors python/geophone_scope/data_store.py (NodeData/DataStore). Buffers
// use fixed-capacity rings for live plotting and optional chunked stores for
// the complete unlimited capture.

import * as cfg from './config.js?v=field-study-44';

/** Fixed-capacity ring buffer over a Float64Array — O(1) push, oldest evicted on overflow. */
export class RingBuffer {
  constructor(capacity) {
    this._cap = Math.max(1, capacity | 0);
    this._buf = new Float64Array(this._cap);
    this._start = 0;
    this._len = 0;
  }

  get length() { return this._len; }
  get capacity() { return this._cap; }

  push(value) {
    const idx = (this._start + this._len) % this._cap;
    let evicted = null;
    if (this._len >= this._cap) evicted = this._buf[this._start];
    this._buf[idx] = value;
    if (this._len < this._cap) this._len++;
    else {
      this._start = (this._start + 1) % this._cap;
    }
    return evicted;
  }

  /** Escribe la muestra i (0 = la mas vieja); para rellenar huecos. */
  setAt(i, value) {
    if (i < 0 || i >= this._len) return false;
    this._buf[(this._start + i) % this._cap] = value;
    return true;
  }

  /** Most recent `n` samples (or all if n >= length), oldest-first, as a new Float64Array. */
  tail(n) {
    const count = Math.min(n, this._len);
    const out = new Float64Array(count);
    const skip = this._len - count;
    for (let i = 0; i < count; i++) {
      out[i] = this._buf[(this._start + skip + i) % this._cap];
    }
    return out;
  }

  /** Entire buffer contents, oldest-first. */
  toArray() { return this.tail(this._len); }

  /** Resize capacity in place, preserving the most recent samples (mirrors NodeData.resize_buf). */
  resize(newCapacity) {
    const kept = this.tail(Math.min(this._len, Math.max(1, newCapacity | 0)));
    this._cap = Math.max(1, newCapacity | 0);
    this._buf = new Float64Array(this._cap);
    this._start = 0;
    this._len = 0;
    for (let i = 0; i < kept.length; i++) this.push(kept[i]);
  }

  clear() {
    this._start = 0;
    this._len = 0;
  }
}

/** Growable Float64 storage in bounded chunks, used only by unlimited captures. */
export class ChunkedFloat64Store {
  constructor(chunkSize = 1_000_000) {
    this.chunkSize = Math.max(1, chunkSize | 0);
    this.chunks = [];
    this.length = 0;
  }

  push(value) {
    const offset = this.length % this.chunkSize;
    if (offset === 0) this.chunks.push(new Float64Array(this.chunkSize));
    this.chunks[this.chunks.length - 1][offset] = value;
    this.length++;
  }

  append(values) {
    let sourceAt = 0;
    while (sourceAt < values.length) {
      const offset = this.length % this.chunkSize;
      if (offset === 0) this.chunks.push(new Float64Array(this.chunkSize));
      const count = Math.min(this.chunkSize - offset, values.length - sourceAt);
      this.chunks[this.chunks.length - 1].set(values.subarray(sourceAt, sourceAt + count), offset);
      sourceAt += count;
      this.length += count;
    }
  }

  setAt(i, value) {
    if (i < 0 || i >= this.length) return false;
    this.chunks[Math.floor(i / this.chunkSize)][i % this.chunkSize] = value;
    return true;
  }

  toArray() {
    const out = new Float64Array(this.length);
    let at = 0;
    for (const chunk of this.chunks) {
      const count = Math.min(chunk.length, this.length - at);
      if (count <= 0) break;
      out.set(count === chunk.length ? chunk : chunk.subarray(0, count), at);
      at += count;
    }
    return out;
  }

  /** Últimas `n` muestras sin materializar el prefijo completo. */
  tail(n) {
    const count = Math.min(Math.max(0, n | 0), this.length);
    const out = new Float64Array(count);
    let dst = count;
    let remaining = count;
    for (let i = this.chunks.length - 1; i >= 0 && remaining > 0; i--) {
      const chunkStart = i * this.chunkSize;
      const used = Math.min(this.chunkSize, this.length - chunkStart);
      const take = Math.min(used, remaining);
      dst -= take;
      out.set(this.chunks[i].subarray(used - take, used), dst);
      remaining -= take;
    }
    return out;
  }

  clear() {
    this.chunks = [];
    this.length = 0;
  }
}

function meanStd(arr) {
  const n = arr.length;
  let sum = 0;
  for (let i = 0; i < n; i++) sum += arr[i];
  const mean = sum / n;
  let sq = 0;
  for (let i = 0; i < n; i++) { const d = arr[i] - mean; sq += d * d; }
  return { mean, std: Math.sqrt(sq / n) };
}

/** Holds all runtime state for one node (master or slave). Mirrors data_store.NodeData. */
export class NodeData {
  /**
   * @param {number} nodeIndex 0-based index matching the protocol node_id (slave_id == nodeIndex).
   * @param {number} maxBuf    Maximum samples in each circular buffer.
   */
  constructor(nodeIndex, maxBuf = cfg.MAX_BUF) {
    this.nodeIndex = nodeIndex;
    this.maxBuf = maxBuf;

    // Voltage sample buffers
    this.rawBuf = new RingBuffer(maxBuf);
    this.filtBuf = new RingBuffer(maxBuf);
    this.fullRawStore = null;
    this.fullFiltStore = null;
    this.rawSum = 0;

    // FIR filter state (populated by signal_proc.js — Phase 4)
    this.filtB = null;     // Float64Array of coefficients
    this.filtZi = null;    // Float64Array filter delay-line state
    this.filtCmd = '';

    // DC removal
    this.dcRemove = false;

    // Cancelación de ruido de línea: ajuste por mínimos cuadrados de armónicos
    // de 50 Hz sobre la captura completa, restado del buffer filtrado (espejo
    // de notch_enabled/notch_harm del scope Python; f0 en cfg.LINE_NOTCH_F0).
    this.notchEnabled = false;
    this.notchHarm = cfg.LINE_NOTCH_DEFAULT_HARM;
    this.cleanI2c = false;
    this.cleanSd = false;
    this.cleanRadio = false;
    this.cleanBeacon = false;
    this.noiseSignaturesPlaca = null;
    this.noiseSignaturesFecha = null;

    // Full-capture filter state. Raw samples may stream/dump incrementally, but
    // FIR/filtFilt and line-noise LS must run only after the node's raw capture
    // is complete.
    this.captureActive = false;
    this.filterDirty = false;
    this.filterReady = false;
    this.filterScope = 'full';
    this.filterStartIndex = 0;
    this.captureTruncated = false;
    this.droppedSamples = 0;

    // Per-node config (updated from heartbeat / ACK packets)
    this.pgaCode = 0;
    this.pgaoutCode = 0;   // etapa PGAout (solo GEO); 0 si el nodo no la tiene
    this.adcConfigCode = 1;
    this.countsPerVolt = cfg.ADC_COUNTS_PER_VOLT;
    this.decimationFactor = 1;   // 1=sin decimar; Fs efectiva = 2604/factor
    this.vdacByte = 128;
    this.calVdacs = Array.from({ length: cfg.CAL_VDAC_STAGE_COUNT }, () => null);
    this.calVdacDetails = Array.from({ length: cfg.CAL_VDAC_STAGE_COUNT }, () => null);
    this.pgavdac = 0;
    this.psocOk = null;   // null until first HELLO
    this.mac = '';
    this.hwClass = 0xFF;  // 0=GEO, 1=HAMMER, 0xFF=unknown (reported by PSoC)
    this.linkModel = 0xFF; // 0=SERIAL, 1=I2C, 0xFF=legacy/unknown (reported by ESP)
    this.sdMarks = [];         // [indice absoluto de muestra, flags] de escrituras a SD
    this.pendingMarkFlags = 0; // marca recibida que aplica a la próxima muestra
    this.ctlStatus = 0xFF; // lazo del PSoC I2C: bit0 en banda, bits1-3 estado, bit4 perfil
    this.sdPresent = false; // FAT/SD detectada por el PSoC (solo esclavos GEO, ver plan_2929_decimation_sd.md)
    this.sdEnabled = false; // estado del toggle SD confirmado por ACK (0xC0)
    this.ramBatches = null; // capacidad HELLO 0x0C; null hasta recibirla
    this.sdBatches = null;  // capacidad HELLO 0x0D; 0 significa sin SD

    // ADC sample rate — depends on the analog front-end / PSoC programming
    // (and can be reconfigured), so it must ALWAYS come from the hardware
    // (HELLO b0 = fs/100). No nominal/guessed value: 0 means "not yet known".
    this.fs = 0;
    this.fsKnown = false;   // true once a HELLO reported fs > 0
    this.fsExactKnown = false; // true once HELLO sub-packet 0x05 reported exact Hz

    // Batch / statistics
    this.batchCount = 0;
    this.totalSamples = 0;
    this.driftHist = [];
    this.latencyHist = [];

    // Acquisition timing helpers (used by test/drift logic)
    this.gotFirst = false;
    this.tFirst = 0;

    // Health (0=unknown, 1=ok, 2=fail)
    this.health = 0;

    // Distance from seismic source (hammer) to this receiver [m]
    this.hammerOffset = 0;

    // Y-axis display offset [V] — added to all curves for visual separation (display only, not exported)
    this.yOffsetV = 0;

    // Invert signal for display (multiply by -1) — e.g. hammer channel which arrives inverted
    this.invertSignal = false;

    // Visibility in plots — master (0) not plotted by default
    this.visible = nodeIndex > 0;

    this.slaveId = nodeIndex === 0 ? 'M' : `S${nodeIndex}`;
    this.alias = nodeIndex === 0 ? 'Maestro' : (cfg.NODE_NAMES[nodeIndex] || `S${nodeIndex}`);

    // Pending commands tracking: subCmd -> { param, sendTime, retries }
    this.pending = new Map();

    // Throttle del log "calibrando..." (heartbeat ok=2 de SUBCMD_CALIBRATE)
    this.calProgressLogMs = 0;
  }

  // ── Helpers ─────────────────────────────────────────────────────────────

  /** Resize circular buffers in-place (preserves most-recent samples). */
  resizeBuf(newMax) {
    this.maxBuf = newMax;
    this.rawBuf.resize(newMax);
    this.filtBuf.resize(newMax);
    this.rawSum = 0;
    const raw = this.rawBuf.toArray();
    for (let i = 0; i < raw.length; i++) this.rawSum += raw[i];
  }

  setFullCaptureEnabled(enabled) {
    if (enabled) {
      this.fullRawStore = new ChunkedFloat64Store();
      this.fullFiltStore = new ChunkedFloat64Store();
    } else {
      this.fullRawStore = null;
      this.fullFiltStore = null;
    }
  }

  get fullCaptureEnabled() { return this.fullRawStore !== null; }
  get captureRawLength() { return this.fullRawStore?.length ?? this.rawBuf.length; }
  get captureFiltLength() { return this.fullFiltStore?.length ?? this.filtBuf.length; }
  captureRawArray() { return this.fullRawStore ? this.fullRawStore.toArray() : this.rawBuf.toArray(); }
  captureFiltArray() { return this.fullFiltStore ? this.fullFiltStore.toArray() : this.filtBuf.toArray(); }

  /** Escribe la muestra de posicion absoluta abs de la captura (relleno). */
  setRawAt(abs, value) {
    let ok = false;
    if (this.fullRawStore) ok = this.fullRawStore.setAt(abs, value);
    const first = this.totalSamples - this.rawBuf.length;
    if (abs >= first) ok = this.rawBuf.setAt(abs - first, value) || ok;
    return ok;
  }

  /** Estado de los lotes numerados (cero perdida): huecos y relleno. */
  resetBatchTracking() {
    this.seqNext = 0;
    this.missingBatches = new Set();
    this.lostBatches = new Set();
    this.fillAsked = new Map();
    this.rxMode = 'append';
    this.fillPos = 0;
    this.fillRemain = 0;
  }

  pushRaw(value) {
    if (this.fullRawStore) this.fullRawStore.push(value);
    return this.rawBuf.push(value);
  }

  setFullFiltered(values) {
    if (!this.fullFiltStore) return;
    this.fullFiltStore.clear();
    this.fullFiltStore.append(values);
  }

  /** Marcas relativas a un arreglo que termina en la última muestra almacenada. */
  sdMarksFor(length) {
    const stored = this.fullRawStore ? this.fullRawStore.length : this.totalSamples;
    const first = Math.max(0, stored - length);
    return this.sdMarks
      .filter(([i]) => i >= first && i < stored)
      .map(([i, f]) => [i - first, f]);
  }

  /** Clear sample buffers and reset filter state. */
  clear() {
    this.resetBatchTracking();
    this.rawBuf.clear();
    this.filtBuf.clear();
    if (this.fullRawStore) this.fullRawStore.clear();
    if (this.fullFiltStore) this.fullFiltStore.clear();
    this.rawSum = 0;
    this.filtZi = null;
    this.liveFir = null;
    this.batchCount = 0;
    this.totalSamples = 0;
    this.sdMarks = [];
    this.pendingMarkFlags = 0;
    this.gotFirst = false;
    this.tFirst = 0;
    this.captureActive = false;
    this.filterDirty = false;
    this.filterReady = false;
    this.filterScope = 'full';
    this.filterStartIndex = 0;
    this.captureTruncated = false;
    this.droppedSamples = 0;
  }

  addDrift(value, maxHist = 50) {
    this.driftHist.push(value);
    if (this.driftHist.length > maxHist) this.driftHist = this.driftHist.slice(-maxHist);
  }

  addLatency(valueUs, maxHist = 50) {
    this.latencyHist.push(valueUs);
    if (this.latencyHist.length > maxHist) this.latencyHist = this.latencyHist.slice(-maxHist);
  }

  /** Concise mean ± std string for the drift history (ms). */
  formatDriftStats() {
    if (this.driftHist.length === 0) return '--';
    const { mean, std } = meanStd(this.driftHist);
    return `${(mean * 1e3).toFixed(1)} ± ${(std * 1e3).toFixed(1)} ms`;
  }

  /** Concise mean ± std string for the latency history (µs). */
  formatLatencyStats() {
    if (this.latencyHist.length === 0) return '--';
    const { mean, std } = meanStd(this.latencyHist);
    return `${mean.toFixed(0)} ± ${std.toFixed(0)} µs`;
  }
}

/** Container for all per-node data objects. Mirrors data_store.DataStore. */
export class DataStore {
  constructor(maxBuf = cfg.MAX_BUF) {
    this.nodes = [];
    for (let i = 0; i < cfg.MAX_NODES; i++) this.nodes.push(new NodeData(i, maxBuf));
  }

  get(idx) { return this.nodes[idx]; }

  /** Clear sample buffers on every node. */
  clearAll() {
    for (const nd of this.nodes) nd.clear();
  }

  /** Resize every node's buffer. */
  resizeAll(newMax) {
    for (const nd of this.nodes) nd.resizeBuf(newMax);
  }

  setFullCaptureEnabled(enabled) {
    for (let i = 1; i < this.nodes.length; i++) this.nodes[i].setFullCaptureEnabled(enabled);
  }
}

/**
 * Sample rate to use for batch/duration/display/export calculations.
 *
 * The ADC rate depends on how the analog front-end / PSoC is programmed and
 * can be changed at any time, so there is NO nominal/guessed fallback: this
 * returns the value reported by the first slave that has sent a HELLO, or
 * 0 if no slave has reported one yet — callers must treat 0 as "unknown"
 * and wait, never substitute a constant. Mirrors MainWindow._effective_fs.
 */
export function effectiveFs(dataStore) {
  for (let i = 1; i < dataStore.nodes.length; i++) {
    const nd = dataStore.nodes[i];
    if (nd.fsKnown) return nd.fs;
  }
  return 0;
}
