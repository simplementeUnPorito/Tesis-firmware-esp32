// data_store.js — per-node circular sample buffers and acquisition stats.
// Mirrors python/geophone_scope/data_store.py (NodeData/DataStore). Buffers
// are backed by a fixed-capacity RingBuffer (Float64Array) instead of a
// maxlen-deque, since JS has no built-in bounded queue with O(1) push+evict.

import * as cfg from './config.js?v=field-loop-18';

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
    this.rawSum = 0;

    // FIR filter state (populated by signal_proc.js — Phase 4)
    this.filtB = null;     // Float64Array of coefficients
    this.filtZi = null;    // Float64Array filter delay-line state
    this.filtCmd = '';

    // DC removal
    this.dcRemove = false;

    // Per-node config (updated from heartbeat / ACK packets)
    this.pgaCode = 0;
    this.vdacByte = 128;
    this.pgavdac = 0;
    this.psocOk = null;   // null until first HELLO
    this.mac = '';

    // ADC sample rate — depends on the analog front-end / PSoC programming
    // (and can be reconfigured), so it must ALWAYS come from the hardware
    // (HELLO b0 = fs/100). No nominal/guessed value: 0 means "not yet known".
    this.fs = 0;
    this.fsKnown = false;   // true once a HELLO reported fs > 0

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

    // Visibility in plots — master (0) not plotted by default
    this.visible = nodeIndex > 0;

    this.slaveId = nodeIndex === 0 ? 'M' : `S${nodeIndex}`;
    this.alias = nodeIndex === 0 ? 'Maestro' : (cfg.SLAVE_TYPE_ORDER[nodeIndex - 1] || `Slave${nodeIndex}`);

    // Pending commands tracking: subCmd -> { param, sendTime, retries }
    this.pending = new Map();
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

  /** Clear sample buffers and reset filter state. */
  clear() {
    this.rawBuf.clear();
    this.filtBuf.clear();
    this.rawSum = 0;
    this.filtZi = null;
    this.batchCount = 0;
    this.totalSamples = 0;
    this.gotFirst = false;
    this.tFirst = 0;
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
