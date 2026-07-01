// app.js - bootstraps the web UI. Mirrors the orchestration role of
// gui/main_window.py: WebSocket packets -> DataStore/UI, and UI actions ->
// the same command bytes that handleMatlabCmd() already consumes.

import * as cfg from './config.js?v=field-study-3';
import { WsClient } from './ws_client.js?v=field-study-3';
import { encodeStd, encodeStd16, encodeDirected } from './protocol.js?v=field-study-3';
import { DataStore, effectiveFs } from './data_store.js?v=field-study-3';
import { PlotArea } from './plot.js?v=field-study-3';
import { SpectrumArea } from './spectrum.js?v=field-study-3';
import { SlavePanel } from './slave_panel.js?v=field-study-3';
import { compileFirCmd, dcRemove, filtFilt, lastFirError } from './signal_proc.js?v=field-study-3';
import { buildCaptureZip, downloadBlob } from './export.js?v=field-study-3';

const $ = (id) => document.getElementById(id);

const ws = new WsClient(`ws://${location.host}/ws`);
const data = new DataStore();
const plotArea = new PlotArea($('plots'));
const spectrumArea = new SpectrumArea($('spectra'));
const slavePanels = new Array(cfg.MAX_NODES).fill(null);
const macPartial = new Map();
const pgaLockTimers = new Map();
const testTimers = new Map();
const preservedCaptures = [];
const PRESERVE_COLORS = [
  'rgba(20, 184, 166, 0.72)',
  'rgba(245, 158, 11, 0.72)',
  'rgba(168, 85, 247, 0.72)',
  'rgba(34, 197, 94, 0.72)',
  'rgba(236, 72, 153, 0.72)',
  'rgba(14, 165, 233, 0.72)',
];

let masterState = 0;
let activeSlaveCount = 0;
let preserveSeq = 0;

const SETTINGS_PREFIX = 'geophone_scope_web.';
const GLOBAL_FS_KEY = `${SETTINGS_PREFIX}fs_hz_v2`;
const SHOW_RAW_KEY = `${SETTINGS_PREFIX}show_raw`;
const SHOW_FILT_KEY = `${SETTINGS_PREFIX}show_filt`;
const SHOW_ENV_RAW_KEY = `${SETTINGS_PREFIX}show_env_raw`;
const SHOW_ENV_FILT_KEY = `${SETTINGS_PREFIX}show_env_filt`;
const SHOW_HILBERT_KEY = `${SETTINGS_PREFIX}show_hilbert`;
const DISPLAY_DC_REMOVE_KEY = `${SETTINGS_PREFIX}display_dc_remove`;
const WS_TOKEN_KEY  = `${SETTINGS_PREFIX}ws_token`;

function clamp(value, lo, hi) {
  return Math.max(lo, Math.min(hi, value));
}

// ── Auth modal ───────────────────────────────────────────────────────────────

function showAuthModal(showError = false) {
  const modal = $('auth-modal');
  if (!modal) return;
  const firstShow = modal.hidden;
  modal.hidden = false;
  const err = $('auth-error');
  if (err) {
    if (showError) err.hidden = false;      // siempre mostrar si hay error
    else if (firstShow) err.hidden = true;  // solo ocultar en primera apertura
  }
  if (firstShow) {
    const inp = $('auth-password');
    if (inp) { inp.value = ''; inp.focus(); }
  }
}

function hideAuthModal() {
  const modal = $('auth-modal');
  if (modal) modal.hidden = true;
}

function applyAuthToken(token) {
  ws.authToken = token || null;
  try {
    if (token) localStorage.setItem(WS_TOKEN_KEY, token);
    else localStorage.removeItem(WS_TOKEN_KEY);
  } catch (_) {}
}

function settingKey(chIndex, name) {
  return `${SETTINGS_PREFIX}slave.${chIndex}.${name}`;
}

function saveSlaveSetting(chIndex, name, value) {
  try {
    localStorage.setItem(settingKey(chIndex, name), String(value));
  } catch (_) {
    // localStorage can be disabled on some phone browser/privacy modes.
  }
}

function loadSetting(key) {
  try {
    return localStorage.getItem(key);
  } catch (_) {
    return null;
  }
}

function loadSlaveInt(chIndex, name, fallback, lo, hi) {
  const raw = loadSetting(settingKey(chIndex, name));
  const parsed = raw === null ? NaN : parseInt(raw, 10);
  if (!Number.isFinite(parsed)) return fallback;
  return clamp(parsed, lo, hi);
}

function loadSlaveFloat(chIndex, name, fallback) {
  const raw = loadSetting(settingKey(chIndex, name));
  const parsed = raw === null ? NaN : parseFloat(raw);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function loadSlaveBool(chIndex, name, fallback) {
  const raw = loadSetting(settingKey(chIndex, name));
  if (raw === null) return fallback;
  return raw === '1' || raw === 'true' || raw === 'on';
}

function loadBoolSetting(key, fallback) {
  const raw = loadSetting(key);
  if (raw === null) return fallback;
  return raw === '1' || raw === 'true' || raw === 'on';
}

function saveSetting(key, value) {
  try {
    localStorage.setItem(key, String(value));
  } catch (_) {
    // localStorage can be disabled on some phone browser/privacy modes.
  }
}

function saveGlobalFs(fsHz) {
  if (!(fsHz > 0)) return;
  try {
    localStorage.setItem(GLOBAL_FS_KEY, String(Math.round(fsHz)));
  } catch (_) {
    // localStorage can be disabled on some phone browser/privacy modes.
  }
}

function anySlaveFsKnown() {
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].fsKnown) return true;
  }
  return false;
}

function hardwareFsHz() {
  return effectiveFs(data);
}

// Returns the hardware Fs when available, otherwise the measured default rate.
// HELLO exact (sub-packet 0x05) replaces this fallback as soon as it arrives.
function currentFsHz() {
  return hardwareFsHz() || cfg.DEFAULT_SAMPLE_RATE_HZ;
}

function displayFsHz() {
  const liveFs = hardwareFsHz();
  if (liveFs) return liveFs;
  for (const capture of selectedPreservedCaptures()) {
    if (capture.fs > 0) return capture.fs;
  }
  for (const capture of preservedCaptures) {
    if (capture.fs > 0) return capture.fs;
  }
  return cfg.DEFAULT_SAMPLE_RATE_HZ;
}

function compactTimestamp(date) {
  const pad = (n) => String(n).padStart(2, '0');
  return `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}_` +
         `${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
}

function humanTimestamp(date) {
  const pad = (n) => String(n).padStart(2, '0');
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

function gainValue(code) {
  return (code >= 0 && code < cfg.GAIN_CODES.length) ? cfg.GAIN_CODES[code] : null;
}

function signalMean(arr) {
  if (!arr || !arr.length) return 0;
  let sum = 0;
  for (let i = 0; i < arr.length; i++) sum += arr[i];
  return sum / arr.length;
}

function displayDcRemoveEnabled() {
  const cb = $('chk-dc-remove');
  return !cb || cb.checked;
}

function transformSignalForView(arr, options = {}) {
  if (!arr || !arr.length) return arr || null;
  const removeDc = !!options.removeDc && arr.length > 1;
  const invert = !!options.invert;
  const yOffsetV = Number.isFinite(options.yOffsetV) ? options.yOffsetV : 0;
  if (!removeDc && !invert && !yOffsetV) return arr;
  const mean = removeDc ? signalMean(arr) : 0;
  const sign = invert ? -1 : 1;
  const out = new Float64Array(arr.length);
  for (let i = 0; i < arr.length; i++) out[i] = (arr[i] - mean) * sign + yOffsetV;
  return out;
}

function nodeConnectedForCapture(nd, index) {
  return index > 0
    && (nd.visible || nd.rawBuf.length > 0 || nd.filtBuf.length > 0)
    && (nd.fsKnown || !!nd.mac || nd.hwClass !== 0xFF || nd.rawBuf.length > 0 || nd.filtBuf.length > 0);
}

function nodeSnapshot(nd, index, options = {}) {
  const rawOrig = nd.rawBuf.toArray();
  const filtOrig = filteredArrayForNode(nd, rawOrig);
  const storeDcRemoved = !!options.storeDcRemoved;
  const rawDc = signalMean(rawOrig);
  const filtDc = signalMean(filtOrig);
  const raw = storeDcRemoved ? transformSignalForView(rawOrig, { removeDc: true }) : rawOrig;
  const filt = storeDcRemoved ? transformSignalForView(filtOrig, { removeDc: true }) : filtOrig;
  return {
    index,
    name: cfg.NODE_NAMES[index] || `Node ${index}`,
    type: nd.alias || cfg.NODE_NAMES[index] || `Node ${index}`,
    slave_id: nd.slaveId,
    fs: nd.fs,
    fs_known: !!nd.fsKnown,
    connected: nodeConnectedForCapture(nd, index),
    raw,
    filt,
    raw_count: raw.length,
    filt_count: filt.length,
    batch_count: nd.batchCount,
    total_samples: nd.totalSamples,
    pga_code: nd.pgaCode,
    pga_gain: gainValue(nd.pgaCode),
    vdac_byte: nd.vdacByte,
    pgavdac_code: nd.pgavdac,
    pgavdac_gain: gainValue(nd.pgavdac),
    psoc_ok: nd.psocOk,
    hw_class: nd.hwClass,
    hw_type: slaveHwClassName(nd.hwClass),
    hammer_offset_m: nd.hammerOffset ?? 0,
    sample_offset: 0,
    mac: nd.mac || '',
    visible: !!nd.visible,
    fir_cmd: nd.filtCmd || '',
    filt_trim_samples: 0,
    dc_remove: !!nd.dcRemove,
    dc_removed_on_preserve: storeDcRemoved,
    raw_dc_v: rawDc,
    filt_dc_v: filtDc,
    invert_signal: !!nd.invertSignal,
    drift_hist: nd.driftHist.slice(),
    latency_hist: nd.latencyHist.slice(),
    health: nd.health,
    units: 'V',
    dtype: 'float32',
    endian: 'little',
  };
}

function captureSampleCount(capture, signal = 'raw') {
  if (!capture || !Array.isArray(capture.nodes)) return 0;
  return capture.nodes.reduce((sum, node) => {
    const values = node ? node[signal] : null;
    return sum + (values && values.length ? values.length : 0);
  }, 0);
}

function captureHasSamples(capture) {
  return captureSampleCount(capture, 'raw') > 0 || captureSampleCount(capture, 'filt') > 0;
}

function captureSignature(nodes) {
  return nodes.map((node) => {
    const raw = node.raw || [];
    const filt = node.filt || [];
    const rawLast = raw.length ? raw[raw.length - 1].toPrecision(8) : '';
    const filtLast = filt.length ? filt[filt.length - 1].toPrecision(8) : '';
    return `${node.index}:${raw.length}:${filt.length}:${node.total_samples}:${rawLast}:${filtLast}:${node.fs}`;
  }).join('|');
}

function captureSampleOffset(capture) {
  return Number.isFinite(capture?.sample_offset) ? Math.round(capture.sample_offset) : 0;
}

function captureYOffsetMv(capture) {
  const mv = Number(capture?.y_offset_mv);
  return Number.isFinite(mv) ? mv : 0;
}

function makeCaptureSnapshot(label, options = {}) {
  const now = new Date();
  const source = options.source || 'preserved';
  const order = Number.isFinite(options.order) ? options.order : 0;
  const storeDcRemoved = options.storeDcRemoved === true;
  const nodes = data.nodes.map((nd, index) => nodeSnapshot(nd, index, { storeDcRemoved }));
  const fs = currentFsHz() || nodes.find((node) => node.fs > 0)?.fs || 0;
  const capture = {
    id: options.id || `${source}_${now.getTime()}_${order || preserveSeq + 1}`,
    order,
    label: label || (source === 'live' ? 'Actual' : `Start ${order}`),
    source,
    created_at: now.toISOString(),
    save_time: compactTimestamp(now),
    display_time: humanTimestamp(now),
    visible: options.visible !== false,
    sample_offset: Number.isFinite(options.sampleOffset) ? Math.round(options.sampleOffset) : 0,
    y_offset_mv: Number.isFinite(options.yOffsetMv) ? options.yOffsetMv : 0,
    dc_removed_on_preserve: storeDcRemoved,
    color: options.color || PRESERVE_COLORS[Math.max(0, order - 1) % PRESERVE_COLORS.length],
    fs,
    n_slaves: visibleSlaveCount(),
    n_batches: captureBatches(),
    samples_per_batch: cfg.SAMPLES_PER_BATCH,
    display_secs: parseInt($('disp-secs').value, 10) || null,
    max_buf_secs: cfg.MAX_BUF_S,
    active_node_indices: orderedSlaveIndices(true),
    nodes,
  };
  capture.raw_count = captureSampleCount(capture, 'raw');
  capture.filt_count = captureSampleCount(capture, 'filt');
  capture.signature = captureSignature(nodes);
  return capture;
}

function selectedPreservedCaptures() {
  return preservedCaptures.filter((capture) => capture.visible);
}

function selectedPreservedNodeIndices() {
  const indices = new Set();
  for (const capture of selectedPreservedCaptures()) {
    for (const node of capture.nodes) {
      if (node && node.index > 0 && (node.raw_count > 0 || node.filt_count > 0)) {
        indices.add(node.index);
      }
    }
  }
  return indices;
}

function refreshPreservedStatus() {
  const el = $('preserve-status');
  if (!el) return;
  const selected = selectedPreservedCaptures().length;
  const rawCount = preservedCaptures.reduce((sum, cap) => sum + cap.raw_count, 0);
  el.textContent = `${preservedCaptures.length} preservados / ${selected} visibles / ${rawCount} muestras raw`;
}

function renderPreservedList() {
  const list = $('preserve-list');
  if (!list) return;
  list.textContent = '';
  const captures = preservedCaptures.slice().sort((a, b) => b.order - a.order);
  for (const capture of captures) {
    const row = document.createElement('div');
    row.className = 'preserve-row';

    const chk = document.createElement('input');
    chk.type = 'checkbox';
    chk.checked = !!capture.visible;
    chk.addEventListener('change', () => {
      capture.visible = chk.checked;
      refreshPreservedStatus();
      refreshSlavePresentationOrder();
    });

    const swatch = document.createElement('span');
    swatch.className = 'swatch';
    swatch.style.background = capture.color;

    const meta = document.createElement('div');
    meta.className = 'meta';
    const title = document.createElement('div');
    title.className = 'title';
    title.textContent = capture.label;
    const sub = document.createElement('div');
    sub.className = 'sub';
    const dcText = capture.dc_removed_on_preserve ? 'sin DC' : 'con DC';
    sub.textContent = `${capture.display_time} · ${capture.raw_count} raw · Fs ${capture.fs ? capture.fs.toFixed(0) : '?'} Hz · ${dcText} · X ${captureSampleOffset(capture)} muestras · Y ${captureYOffsetMv(capture)} mV`;
    meta.append(title, sub);

    const offsetWrap = document.createElement('label');
    offsetWrap.className = 'preserve-offset';
    offsetWrap.appendChild(document.createTextNode('Offset X '));
    const offsetInput = document.createElement('input');
    offsetInput.type = 'number';
    offsetInput.step = '1';
    offsetInput.value = String(captureSampleOffset(capture));
    offsetInput.title = 'Mueve esta captura en el eje X, en muestras. Acepta valores positivos o negativos.';
    offsetInput.addEventListener('change', () => {
      const parsed = parseInt(offsetInput.value, 10);
      capture.sample_offset = Number.isFinite(parsed) ? parsed : 0;
      renderPreservedList();
      refreshSlavePresentationOrder();
    });
    offsetWrap.appendChild(offsetInput);

    const yOffsetWrap = document.createElement('label');
    yOffsetWrap.className = 'preserve-offset';
    yOffsetWrap.appendChild(document.createTextNode('Offset Y mV '));
    const yOffsetInput = document.createElement('input');
    yOffsetInput.type = 'number';
    yOffsetInput.step = '100';
    yOffsetInput.value = String(captureYOffsetMv(capture));
    yOffsetInput.title = 'Desplaza verticalmente esta captura preservada despues de quitar DC.';
    yOffsetInput.addEventListener('change', () => {
      const parsed = parseFloat(yOffsetInput.value);
      capture.y_offset_mv = Number.isFinite(parsed) ? parsed : 0;
      renderPreservedList();
      refreshSlavePresentationOrder();
    });
    yOffsetWrap.appendChild(yOffsetInput);

    const del = document.createElement('button');
    del.type = 'button';
    del.textContent = 'Quitar';
    del.addEventListener('click', () => {
      const idx = preservedCaptures.findIndex((item) => item.id === capture.id);
      if (idx >= 0) preservedCaptures.splice(idx, 1);
      renderPreservedList();
      refreshSlavePresentationOrder();
    });

    row.append(chk, swatch, meta, offsetWrap, yOffsetWrap, del);
    list.appendChild(row);
  }
  refreshPreservedStatus();
}

function setAllPreservedVisible(visible) {
  for (const capture of preservedCaptures) capture.visible = !!visible;
  renderPreservedList();
  refreshSlavePresentationOrder();
}

function clearPreservedCaptures() {
  preservedCaptures.length = 0;
  renderPreservedList();
  refreshSlavePresentationOrder();
}

function onPreserveRequested() {
  const nextOrder = preserveSeq + 1;
  const capture = makeCaptureSnapshot(`Start ${nextOrder}`, {
    source: 'preserved',
    order: nextOrder,
    visible: true,
    storeDcRemoved: displayDcRemoveEnabled(),
  });
  if (!captureHasSamples(capture)) {
    appendLog('Preservar cancelado: no hay muestras en el buffer actual');
    return;
  }
  preserveSeq = nextOrder;
  preservedCaptures.push(capture);
  renderPreservedList();
  refreshSlavePresentationOrder();
  appendLog(`Preservado ${capture.label}: ${capture.raw_count} muestras raw`);
}

function captureSeconds() {
  const el = $('capture-secs');
  const parsed = el ? parseFloat(el.value) : NaN;
  const secs = Number.isFinite(parsed) ? parsed : 3.0;
  return clamp(secs, 0.1, 120);
}

// Returns 0 (meaning "cannot compute yet") when Fs is still unknown — the
// caller must check for that instead of silently capturing with a wrong rate.
function batchesForSeconds(seconds) {
  const fs = currentFsHz();
  if (!fs) return 0;
  const batches = Math.ceil((seconds * fs) / cfg.SAMPLES_PER_BATCH);
  return clamp(Math.max(1, batches), 1, cfg.PSOC_CAPTURE_MAX_BATCHES);
}

function captureLimitInfo(seconds) {
  const fs = currentFsHz();
  if (!fs) return { n: 0, actualSecs: 0, capped: false };
  const requested = Math.max(1, Math.ceil((seconds * fs) / cfg.SAMPLES_PER_BATCH));
  const n = clamp(requested, 1, cfg.PSOC_CAPTURE_MAX_BATCHES);
  return {
    n,
    actualSecs: secondsForBatches(n),
    capped: requested > n,
  };
}

function captureBatches() {
  return batchesForSeconds(captureSeconds());
}

function secondsForBatches(nBatches) {
  const fs = currentFsHz();
  return fs ? (nBatches * cfg.SAMPLES_PER_BATCH) / fs : 0;
}

function updateCapturePreview() {
  const el = $('capture-batches');
  if (!el) return;
  const fs = currentFsHz();
  if (!fs) {
    el.textContent = 'esperando Fs del esclavo (HELLO)…';
    return;
  }
  const secs = captureSeconds();
  const info = captureLimitInfo(secs);
  const capped = info.capped ? ` (max; pedido ${secs.toFixed(2)} s)` : '';
  el.textContent = `${info.n} lotes / real ${info.actualSecs.toFixed(2)} s${capped}`;
}

function syncDisplayWindowToCaptureDuration() {
  const displayEl = $('disp-secs');
  if (!displayEl) return;
  const secs = captureSeconds();
  displayEl.value = secs.toFixed(secs % 1 === 0 ? 0 : 1);
  applyDisplayWindow();
}

function updateGlobalFsDisplay() {
  const el = $('global-fs');
  if (el) {
    const fs = currentFsHz();
    el.textContent = fs ? `${fs.toFixed(0)} Hz` : 'esperando…';
  }
  updateCapturePreview();
}

function panelFor(chIndex) {
  return slavePanels[chIndex] || null;
}

function sendStd(cmd, param = 0) {
  ws.send(encodeStd(cmd, param));
}

function sendStd16(cmd, value = 0) {
  ws.send(encodeStd16(cmd, value));
}

function sendDirected(chIndex, subCmd, param = 0) {
  ws.send(encodeDirected(chIndex, subCmd, param));
}

function setConnIndicator(isConnected) {
  const el = $('conn-status');
  el.textContent = isConnected ? 'conectado' : 'desconectado';
  el.className = isConnected ? 'ok' : 'bad';
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const panel = panelFor(i);
    if (panel) panel.setConnected(isConnected);
  }
}

function setMasterState(stateCode) {
  const name = cfg.MASTER_STATE_NAMES[stateCode] ?? `(${stateCode})`;
  $('master-state').textContent = name;
}

function appendLog(msg) {
  const log = $('log');
  const line = document.createElement('div');
  line.textContent = msg;
  log.appendChild(line);
  while (log.childElementCount > 200) log.removeChild(log.firstChild);
  log.scrollTop = log.scrollHeight;
}

function createNodeRow(i) {
  const row = document.createElement('div');
  row.className = 'node-row';
  row.id = `node-${i}`;
  for (const [className, text] of [
    ['name', cfg.NODE_NAMES[i] || `Esclavo ${i}`],
    ['raw', '--'],
    ['stats', '--'],
    ['cfg', '--'],
  ]) {
    const span = document.createElement('span');
    span.className = className;
    span.textContent = text;
    row.appendChild(span);
  }
  return row;
}

function buildNodeRows() {
  const list = document.querySelector('.node-list');
  if (!list) return;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (!$(`node-${i}`)) list.appendChild(createNodeRow(i));
  }
}

async function resetWsLock() {
  try {
    await fetch('/ws-reset', { cache: 'no-store' });
    appendLog('WS lock liberado; reconectando');
  } catch (_) {
    appendLog('WS lock reset fallo; intento reconectar igual');
  }
}

function slaveTypeRank(alias) {
  const idx = cfg.SLAVE_TYPE_ORDER.indexOf(String(alias || '').trim());
  return idx >= 0 ? idx : cfg.SLAVE_TYPE_ORDER.length;
}

function compareSlaveIndices(a, b) {
  const ar = slaveTypeRank(data.nodes[a]?.alias);
  const br = slaveTypeRank(data.nodes[b]?.alias);
  return (ar - br) || (a - b);
}

function orderedSlaveIndices(visibleOnly = false) {
  const indices = [];
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (!visibleOnly || data.nodes[i].visible) indices.push(i);
  }
  indices.sort(compareSlaveIndices);
  return indices;
}

function nodeTitle(i) {
  if (i <= 0) return cfg.NODE_NAMES[i] || 'Maestro';
  const alias = data.nodes[i].alias || cfg.NODE_NAMES[i] || `S${i}`;
  return `${alias} (S${i})`;
}

function slaveHwClassName(hwClass) {
  if (hwClass === 0) return 'GEO';
  if (hwClass === 1) return 'HAMMER';
  return 'UNKNOWN';
}

function applyReportedSlaveType(chIndex, hwClass) {
  if (chIndex <= 0 || chIndex >= cfg.MAX_NODES) return;
  if (hwClass !== 0 && hwClass !== 1) return;

  const nd = data.nodes[chIndex];
  const panel = panelFor(chIndex);
  nd.hwClass = hwClass;

  if (hwClass === 1) {
    nd.alias = 'Hammer';
    nd.hammerOffset = 0;
    if (panel) {
      panel.setAlias('Hammer');
      panel.setOffset(0);
      panel.setDisplayName(`Hammer (${cfg.NODE_NAMES[chIndex]})`);
    }
  } else {
    if (nd.alias === 'Hammer' || !String(nd.alias || '').startsWith('Geo')) {
      nd.alias = `Geo${chIndex}`;
    }
    if (panel) panel.setAlias(nd.alias);
    reorderGeosByOffset();
  }

  renderNodeRow(chIndex);
  refreshSlavePresentationOrder();
}

function nodeHasPresence(i) {
  const nd = data.nodes[i];
  return !!nd.mac
    || nd.psocOk !== null
    || nd.hwClass !== 0xFF
    || nd.fsExactKnown
    || nd.rawBuf.length > 0
    || nd.filtBuf.length > 0;
}

function visibleSlaveCount() {
  let count = 0;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].visible) count++;
  }
  return count;
}

function applySlaveVisibility(visibleSet) {
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const visible = visibleSet.has(i);
    data.nodes[i].visible = visible;
    const row = $(`node-${i}`);
    if (row) row.hidden = !visible;
    const panel = panelFor(i);
    if (panel) panel.setVisible(visible);
  }
}

function rebalanceSlaveVisibility(preferredIndex = 0) {
  const visibleSet = new Set();
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (nodeHasPresence(i)) visibleSet.add(i);
  }
  if (preferredIndex > 0 && preferredIndex < cfg.MAX_NODES) visibleSet.add(preferredIndex);

  const target = Math.max(activeSlaveCount, visibleSet.size);
  for (let i = 1; visibleSet.size < target && i < cfg.MAX_NODES; i++) {
    if (!visibleSet.has(i)) visibleSet.add(i);
  }

  applySlaveVisibility(visibleSet);
  refreshSlavePresentationOrder();
  updateCapturePreview();
}

function refreshSlavePresentationOrder() {
  const titles = data.nodes.map((_, i) => nodeTitle(i));
  plotArea.setNodeTitles(titles);
  spectrumArea.setNodeTitles(titles);

  const preservedNodes = selectedPreservedNodeIndices();
  const visibleNodeSet = new Set(orderedSlaveIndices(true));
  for (const idx of preservedNodes) visibleNodeSet.add(idx);
  const visibleNodes = orderedSlaveIndices(false).filter((idx) => visibleNodeSet.has(idx));
  plotArea.setActiveNodes(visibleNodes);
  spectrumArea.setActiveNodes(visibleNodes);

  const nodeList = document.querySelector('.node-list');
  if (nodeList) {
    for (const idx of orderedSlaveIndices(false)) {
      const row = $(`node-${idx}`);
      if (row) nodeList.appendChild(row);
    }
  }
}

function nodeIndexByType(type) {
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].alias === type) return i;
  }
  return -1;
}

function formatVelocity(v) {
  if (!(v > 0)) return '';
  if (v >= 1000) return `v≈${(v / 1000).toFixed(2)} km/s`;
  if (v >= 100) return `v≈${v.toFixed(0)} m/s`;
  if (v >= 10) return `v≈${v.toFixed(1)} m/s`;
  return `v≈${v.toFixed(2)} m/s`;
}

function setWaveVelocityReadout(text) {
  const el = $('wave-velocity');
  if (el) el.textContent = text || '';
}

function updateWaveVelocityLabel(fs) {
  const [c1, c2] = plotArea.getCursorSamples();
  const geo1Idx = nodeIndexByType('Geo1');
  const geo2Idx = nodeIndexByType('Geo2');
  if (!(fs > 0) || c1 === null || c2 === null || geo1Idx < 0 || geo2Idx < 0) {
    setWaveVelocityReadout('');
    return;
  }

  const geo1Offset = data.nodes[geo1Idx].hammerOffset;
  const geo2Offset = data.nodes[geo2Idx].hammerOffset;
  const distanceM = Math.abs(geo2Offset - geo1Offset);
  const dtS = Math.abs(c2 - c1) / fs;
  if (!(distanceM > 0) || !(dtS > 0)) {
    setWaveVelocityReadout('');
    return;
  }

  setWaveVelocityReadout(formatVelocity(distanceM / dtS));
}

function setActiveSlaveCount(nSlaves, zeroMeansAll = false) {
  const maxSlaves = cfg.MAX_NODES - 1;
  const parsed = Number.isFinite(nSlaves) ? nSlaves : 0;
  const count = (zeroMeansAll && parsed === 0)
    ? maxSlaves
    : clamp(parsed, 0, maxSlaves);
  activeSlaveCount = count;
  rebalanceSlaveVisibility();
}

function ensureSlaveVisible(chIndex) {
  if (chIndex <= 0 || chIndex >= cfg.MAX_NODES) return;
  rebalanceSlaveVisibility(chIndex);
}

function applyDisplayWindow() {
  const fs = displayFsHz();
  if (!fs) return;   // nothing sensible to size the window to yet
  const secs = parseInt($('disp-secs').value, 10) || 1;
  plotArea.setDisplaySamples(Math.round(secs * fs));
  updateCapturePreview();
}

function syncDataBufferForFs() {
  const fs = currentFsHz();
  if (!fs) return;   // wait for the real Fs — never size buffers off a guess
  const maxSamples = Math.max(
    cfg.SAMPLES_PER_BATCH,
    Math.round(cfg.MAX_BUF_S * fs),
  );
  data.resizeAll(maxSamples);
}

function renderNodeRow(i) {
  if (i <= 0 || i >= cfg.MAX_NODES) return;
  const nd = data.nodes[i];
  const row = $(`node-${i}`);
  if (!row) return;
  row.querySelector('.name').textContent = nodeTitle(i);
  const tail = nd.rawBuf.tail(1);
  const raw = tail.length ? tail[0] : null;
  row.querySelector('.raw').textContent = (raw === null) ? '--' : `${raw.toFixed(4)} V`;
  row.querySelector('.stats').textContent = `${nd.batchCount} lotes / ${nd.totalSamples} muestras`;
  row.querySelector('.cfg').textContent =
    `pga=${cfg.GAIN_NAMES[nd.pgaCode] ?? nd.pgaCode}` +
    (nd.psocOk === null ? '' : ` psoc=${nd.psocOk ? 'ok' : 'no'}`);
}

function updateSlavePanelStats(chIndex) {
  const panel = panelFor(chIndex);
  if (!panel) return;
  const nd = data.nodes[chIndex];
  const tail = nd.rawBuf.tail(1);
  panel.updateStats(
    nd.batchCount,
    nd.totalSamples,
    tail.length ? tail[0] : null,
    nd.formatDriftStats(),
    nd.formatLatencyStats(),
    nd.psocOk,
  );
  if (nd.rawBuf.length > 10) panel.setDcValue(nd.rawSum / nd.rawBuf.length);
  else panel.setDcValue(null);
}

function adcCountsToVolts(counts) {
  return counts / cfg.ADC_COUNTS_PER_VOLT;
}

function handleData(nd, pkt) {
  const rawVal = adcCountsToVolts(pkt.value24);

  if (!nd.gotFirst) {
    nd.gotFirst = true;
    nd.tFirst = performance.now() / 1000;
  }

  const evicted = nd.rawBuf.push(rawVal);
  nd.rawSum += rawVal - (evicted ?? 0);
  nd.totalSamples++;
  if (nd.totalSamples % cfg.SAMPLES_PER_BATCH === 0) nd.batchCount++;

  // Filtered signal is computed via filtFilt (zero-phase) at render time from rawBuf.
  // No per-sample causal filtering here — that would introduce group delay.
}

function fillRingBuffer(ring, arr) {
  ring.clear();
  for (let i = 0; i < arr.length; i++) ring.push(arr[i]);
}

function filteredArrayForNode(nd, rawArray = null) {
  const raw = rawArray || nd.rawBuf.toArray();
  if (!raw.length) return new Float64Array(0);
  let arr = nd.filtB ? filtFilt(nd.filtB, raw) : new Float64Array(raw);
  if (nd.dcRemove && arr.length > 1) arr = dcRemove(arr);
  return arr;
}

function syncZeroPhaseFiltBuffers() {
  for (const nd of data.nodes) {
    fillRingBuffer(nd.filtBuf, filteredArrayForNode(nd));
    nd.filtZi = null;
  }
}

function reprocessFiltBuf(chIndex) {
  const nd = data.nodes[chIndex];
  const raw = nd.rawBuf.toArray();
  if (!raw.length) {
    nd.filtBuf.clear();
    nd.filtZi = null;
    return;
  }

  const arr = filteredArrayForNode(nd, raw);
  nd.filtZi = null;
  fillRingBuffer(nd.filtBuf, arr);
}

function recompileNodeFirForFs(chIndex) {
  const nd = data.nodes[chIndex];
  if (!nd.filtCmd) return true;
  const panel = panelFor(chIndex);
  if (!(nd.fs > 0)) {
    if (panel) panel.setFirStatus(`Pendiente Fs: ${nd.filtCmd}`.slice(0, 80));
    return false;
  }
  const b = compileFirCmd(nd.filtCmd, nd.fs);
  if (!b) {
    nd.filtB = null;
    nd.filtZi = null;
    if (panel) panel.setFirStatus(`Invalid fs=${nd.fs}: ${lastFirError()}`.slice(0, 80));
    appendLog(`S${chIndex} FIR invalido tras fs=${nd.fs}: ${lastFirError()}`);
    reprocessFiltBuf(chIndex);
    return false;
  }
  nd.filtB = b;
  nd.filtZi = null;
  if (panel) panel.setFirStatus(`${b.length} taps ${nd.filtCmd}`);
  reprocessFiltBuf(chIndex);
  return true;
}

function applyGlobalFs(fsHz, logMessage = '') {
  if (!(fsHz > 0)) return;
  const wasKnown = anySlaveFsKnown();
  let changed = false;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const nd = data.nodes[i];
    const nodeChanged = !nd.fsKnown || Math.abs(nd.fs - fsHz) > 1e-9;
    nd.fs = fsHz;
    nd.fsKnown = true;
    if (nodeChanged) {
      recompileNodeFirForFs(i);
      changed = true;
    }
  }
  const manualInput = $('manual-fs-hz');
  if (manualInput) manualInput.value = String(Math.round(fsHz));
  if (!changed && wasKnown) return;

  syncDataBufferForFs();
  applyDisplayWindow();
  updateGlobalFsDisplay();
  for (let i = 1; i < cfg.MAX_NODES; i++) renderNodeRow(i);
  if (logMessage) appendLog(logMessage);
}

function hasExactFsReport() {
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].fsExactKnown) return true;
  }
  return false;
}

function updateNodeFsFromHello(chIndex, fsHz) {
  if (!(fsHz > 0)) return;
  if (hasExactFsReport()) return;
  saveSlaveSetting(chIndex, 'fs_hz', Math.round(fsHz));
  saveGlobalFs(fsHz);
  applyGlobalFs(fsHz, `Fs global actualizado por S${chIndex}: ${fsHz} Hz`);
}

function updateNodeExactFsFromHello(chIndex, fsHz) {
  if (!(fsHz > 0)) return;
  const nd = data.nodes[chIndex];
  if (nd) nd.fsExactKnown = true;
  saveSlaveSetting(chIndex, 'fs_hz', Math.round(fsHz));
  saveGlobalFs(fsHz);
  applyGlobalFs(fsHz, `Fs global actualizado por S${chIndex}: ${fsHz} Hz`);
}

function restoreFsFromLocalCache() {
  const raw = loadSetting(GLOBAL_FS_KEY);
  const fsHz = raw === null ? NaN : parseFloat(raw);
  if (!(fsHz > 0)) return;
  applyGlobalFs(fsHz, `Fs restaurado de cache local: ${fsHz} Hz`);
  const manualInput = $('manual-fs-hz');
  if (manualInput) manualInput.value = String(Math.round(fsHz));
}

function schedulePgaLockTimeout(chIndex, expectedCode) {
  if (pgaLockTimers.has(chIndex)) clearTimeout(pgaLockTimers.get(chIndex));
  const timer = setTimeout(() => {
    pgaLockTimers.delete(chIndex);
    const nd = data.nodes[chIndex];
    const pending = nd.pending.get(cfg.SUBCMD_PGA);
    if (!pending || pending.param !== expectedCode) return;
    nd.pending.delete(cfg.SUBCMD_PGA);
    const panel = panelFor(chIndex);
    if (panel) panel.setPgaLock(2);
    appendLog(`S${chIndex} PGA sin confirmacion: ${cfg.GAIN_NAMES[expectedCode] ?? expectedCode}`);
  }, Math.round(cfg.RETRY_SEC * 1000));
  pgaLockTimers.set(chIndex, timer);
}

function prepareNodeCapture(chIndex) {
  const nd = data.nodes[chIndex];
  nd.clear();
  rebalanceSlaveVisibility(chIndex);
  plotArea.clearNode(chIndex);
  renderNodeRow(chIndex);
  updateSlavePanelStats(chIndex);
  return nd;
}

function onPgaChanged(chIndex, pgaCode) {
  const nd = data.nodes[chIndex];
  nd.pgaCode = clamp(pgaCode, 0, cfg.GAIN_CODES.length - 1);
  nd.pending.set(cfg.SUBCMD_PGA, { param: nd.pgaCode, sendTime: performance.now(), retries: 0 });
  saveSlaveSetting(chIndex, 'pga_code', nd.pgaCode);
  const panel = panelFor(chIndex);
  if (panel) panel.setPgaLock(2);
  sendDirected(chIndex, cfg.SUBCMD_PGA, nd.pgaCode);
  schedulePgaLockTimeout(chIndex, nd.pgaCode);
  appendLog(`S${chIndex} PGA -> ${cfg.GAIN_NAMES[nd.pgaCode]}`);
}

function onVerRequested(chIndex) {
  if (!currentFsHz()) {
    appendLog(`VER Slave ${chIndex} cancelado: esperando Fs real del esclavo (HELLO no recibido aun)`);
    return;
  }
  const n = captureBatches();
  const secs = secondsForBatches(n);
  prepareNodeCapture(chIndex);
  sendStd16(cfg.CMD_SET_RECLEN, n);
  sendDirected(chIndex, cfg.SUBCMD_VER, 1);
  appendLog(`VER Slave ${chIndex}: ${n} lotes (~${secs.toFixed(2)} s real)`);
}

function stopTest(chIndex, initBatchCount = null) {
  if (testTimers.has(chIndex)) {
    clearTimeout(testTimers.get(chIndex));
    testTimers.delete(chIndex);
  }
  sendDirected(chIndex, cfg.SUBCMD_DEBUG, 0);
  sendStd(cfg.CMD_STREAM, 0);
  if (initBatchCount !== null) {
    const gained = data.nodes[chIndex].batchCount - initBatchCount;
    appendLog(`Test Slave ${chIndex}: ${gained >= 2 ? 'OK' : 'FAIL'} (${gained} lotes)`);
  } else {
    appendLog(`Test Slave ${chIndex}: stop`);
  }
}

function onTestRequested(chIndex) {
  if (testTimers.has(chIndex)) {
    stopTest(chIndex);
    return;
  }
  const nd = prepareNodeCapture(chIndex);
  const initBatchCount = nd.batchCount;
  sendStd16(cfg.CMD_SET_RECLEN, 0);
  sendDirected(chIndex, cfg.SUBCMD_DEBUG, 1);
  sendStd(cfg.CMD_STREAM, 1);
  appendLog(`Test Slave ${chIndex}: ${cfg.TEST_DEFAULT_SECONDS.toFixed(1)} s`);
  const timer = setTimeout(
    () => stopTest(chIndex, initBatchCount),
    Math.round(cfg.TEST_DEFAULT_SECONDS * 1000),
  );
  testTimers.set(chIndex, timer);
}

function onSendAll(chIndex) {
  const nd = data.nodes[chIndex];
  sendDirected(chIndex, cfg.SUBCMD_PGA, nd.pgaCode);
  nd.pending.set(cfg.SUBCMD_PGA, { param: nd.pgaCode, sendTime: performance.now(), retries: 0 });
  const panel = panelFor(chIndex);
  if (panel) {
    panel.setPgaLock(2);
  }
  schedulePgaLockTimeout(chIndex, nd.pgaCode);
  appendLog(`S${chIndex} enviar config: PGA ${cfg.GAIN_NAMES[nd.pgaCode] ?? nd.pgaCode}`);
}

function onCalibrateRequested(chIndex) {
  const nd = data.nodes[chIndex];
  sendDirected(chIndex, cfg.SUBCMD_CALIBRATE, 1);
  nd.pending.set(cfg.SUBCMD_CALIBRATE, { param: 1, sendTime: performance.now(), retries: 0 });
  nd.calProgressLogMs = 0;
  const panel = panelFor(chIndex);
  if (panel) panel.setCalibrationLock(3, 'solicitada');
  appendLog(`S${chIndex} calibracion solicitada`);
}

function onBlinkLedRequested(chIndex) {
  sendDirected(chIndex, cfg.SUBCMD_BLINK_LED, 0);
  appendLog(`S${chIndex} titilar LED`);
}

function onSaveEepromRequested(chIndex) {
  sendDirected(chIndex, cfg.SUBCMD_SAVE_EEPROM, 0);
  const panel = panelFor(chIndex);
  if (panel) panel.setEepromLock(0);
  appendLog(`S${chIndex} guardar EEPROM solicitado`);
}

function onFirHwToggled(chIndex, mode) {
  sendDirected(chIndex, cfg.SUBCMD_SELECT_STREAM, mode);
  appendLog(`S${chIndex} stream: ${mode ? 'FIR hardware' : 'ADC crudo'}`);
}

function onLatencyRequested(chIndex) {
  const nd = data.nodes[chIndex];
  nd.latencyHist = [];
  updateSlavePanelStats(chIndex);
  const gapMs = Math.max(1, Math.round(cfg.START_LATENCY_PROBE_GAP_S * 1000));
  for (let k = 0; k < cfg.START_LATENCY_PROBES; k++) {
    setTimeout(() => {
      if (ws.connected) sendDirected(chIndex, cfg.SUBCMD_LATENCY, k & 0xFF);
    }, k * gapMs);
  }
  appendLog(`Latency probe -> Slave ${chIndex} (${cfg.START_LATENCY_PROBES})`);
}

function onExportRequested() {
  try {
    const displaySecs = parseInt($('disp-secs').value, 10) || null;
    const nBatches = captureBatches();
    const baseName = ($('export-name').value || cfg.DEFAULT_SAVE_NAME).trim() || cfg.DEFAULT_SAVE_NAME;
    syncZeroPhaseFiltBuffers();
    const liveCapture = makeCaptureSnapshot('Actual', { source: 'live', visible: false });
    const { blob, filename, metadata } = buildCaptureZip(data, {
      baseName,
      nSlaves: visibleSlaveCount(),
      nBatches,
      displaySecs,
      preservedCaptures: preservedCaptures.slice(),
      liveCapture: captureHasSamples(liveCapture) ? liveCapture : null,
    });
    downloadBlob(blob, filename);
    const captureCount = metadata.capture_count ?? 1;
    const combined = metadata.combined_csv_file ? ` · ${metadata.combined_csv_file}` : '';
    $('export-status').textContent =
      `ZIP: ${filename} (${captureCount} capturas${combined})`;
    appendLog(`Export ZIP ${filename}: ${captureCount} capturas`);
  } catch (err) {
    const msg = err && err.message ? err.message : String(err);
    $('export-status').textContent = `Error: ${msg}`;
    appendLog(`Export error: ${msg}`);
  }
}

function saveFirPreset(chIndex, panel) {
  const preset = panel.firPreset;
  saveSlaveSetting(chIndex, 'fir_type', preset.type);
  saveSlaveSetting(chIndex, 'fir_f1', preset.f1);
  saveSlaveSetting(chIndex, 'fir_f2', preset.f2);
  saveSlaveSetting(chIndex, 'fir_taps', preset.taps);
}

function onFirApply(chIndex, cmd) {
  const nd = data.nodes[chIndex];
  const b = compileFirCmd(cmd, nd.fs);
  const panel = panelFor(chIndex);
  if (!b) {
    const err = lastFirError();
    if (panel) panel.setFirStatus(`Invalid: ${err || 'cmd'}`.slice(0, 80));
    appendLog(`S${chIndex} FIR invalido: ${err || cmd}`);
    return;
  }
  nd.filtB = b;
  nd.filtZi = null;
  nd.filtCmd = cmd;
  reprocessFiltBuf(chIndex);
  saveSlaveSetting(chIndex, 'fir_cmd', cmd);
  if (panel) {
    panel.setFirStatus(`${b.length} taps ${cmd}`);
    saveFirPreset(chIndex, panel);
  }
  appendLog(`S${chIndex} FIR aplicado: ${cmd}`);
}

function onFirRemove(chIndex) {
  const nd = data.nodes[chIndex];
  nd.filtB = null;
  nd.filtZi = null;
  nd.filtCmd = '';
  reprocessFiltBuf(chIndex);
  saveSlaveSetting(chIndex, 'fir_cmd', '');
  const panel = panelFor(chIndex);
  if (panel) panel.setFirStatus('No filter');
  appendLog(`S${chIndex} FIR removido`);
}

function onDcRemoveToggled(chIndex, enabled) {
  const nd = data.nodes[chIndex];
  nd.dcRemove = !!enabled;
  reprocessFiltBuf(chIndex);
  saveSlaveSetting(chIndex, 'dc_remove', nd.dcRemove ? 1 : 0);
}

function loadSlavePanelState(chIndex, panel) {
  const nd = data.nodes[chIndex];
  const alias = loadSetting(settingKey(chIndex, 'alias'));
  if (alias) {
    nd.alias = alias;
    panel.setAlias(alias);
  }

  nd.pgaCode = loadSlaveInt(chIndex, 'pga_code', nd.pgaCode, 0, cfg.GAIN_CODES.length - 1);
  nd.dcRemove = loadSlaveBool(chIndex, 'dc_remove', nd.dcRemove);

  panel.setPga(nd.pgaCode);
  panel.setDcRemove(nd.dcRemove);
  nd.hammerOffset = loadSlaveFloat(chIndex, 'hammer_offset_m', 0);
  panel.setOffset(nd.hammerOffset);

  nd.yOffsetV = loadSlaveFloat(chIndex, 'y_offset_mv', 0) / 1000;
  panel.setYOffset(nd.yOffsetV * 1000);

  nd.invertSignal = loadSlaveBool(chIndex, 'invert_signal', false);
  panel.setInvertSignal(nd.invertSignal);

  panel.setFirPreset(
    loadSetting(settingKey(chIndex, 'fir_type')) || 'lp',
    loadSlaveFloat(chIndex, 'fir_f1', 10),
    loadSlaveFloat(chIndex, 'fir_f2', 200),
    loadSlaveInt(chIndex, 'fir_taps', 101, 3, 401),
  );
  const firCmd = loadSetting(settingKey(chIndex, 'fir_cmd')) || '';
  if (firCmd.trim()) {
    nd.filtCmd = firCmd;
    recompileNodeFirForFs(chIndex);
  }
  panel.updateStats(0, 0, null, nd.formatDriftStats(), nd.formatLatencyStats(), nd.psocOk);
}

function wireSlavePanel(chIndex, panel) {
  panel.addEventListener('alias-changed', (ev) => {
    data.nodes[chIndex].alias = ev.detail;
    saveSlaveSetting(chIndex, 'alias', ev.detail);
    renderNodeRow(chIndex);
    refreshSlavePresentationOrder();
    appendLog(`Orden de salida: ${orderedSlaveIndices(true).map((idx) => nodeTitle(idx)).join(', ') || '--'}`);
  });
  panel.addEventListener('pga-changed', (ev) => onPgaChanged(chIndex, ev.detail));
  panel.addEventListener('fir-apply', (ev) => onFirApply(chIndex, ev.detail.cmd));
  panel.addEventListener('fir-remove', () => onFirRemove(chIndex));
  panel.addEventListener('dc-remove-toggled', (ev) => onDcRemoveToggled(chIndex, !!ev.detail.enabled));
  panel.addEventListener('test-requested', () => onTestRequested(chIndex));
  panel.addEventListener('ver-requested', () => onVerRequested(chIndex));
  panel.addEventListener('send-all-requested', () => onSendAll(chIndex));
  panel.addEventListener('calibrate-requested', () => onCalibrateRequested(chIndex));
  panel.addEventListener('latency-requested', () => onLatencyRequested(chIndex));
  panel.addEventListener('blink-led-requested', () => onBlinkLedRequested(chIndex));
  panel.addEventListener('save-eeprom-requested', () => onSaveEepromRequested(chIndex));
  panel.addEventListener('fir-hw-toggled', (ev) => onFirHwToggled(chIndex, ev.detail.mode));
  panel.addEventListener('offset-changed', (ev) => {
    data.nodes[chIndex].hammerOffset = ev.detail;
    saveSlaveSetting(chIndex, 'hammer_offset_m', ev.detail);
    // Re-sort geo display names by distance whenever any offset changes.
    reorderGeosByOffset();
  });
  panel.addEventListener('y-offset-changed', (ev) => {
    const mv = ev.detail || 0;
    data.nodes[chIndex].yOffsetV = mv / 1000;
    saveSlaveSetting(chIndex, 'y_offset_mv', mv);
  });
  panel.addEventListener('invert-toggled', (ev) => {
    data.nodes[chIndex].invertSignal = !!ev.detail.enabled;
    saveSlaveSetting(chIndex, 'invert_signal', data.nodes[chIndex].invertSignal ? 1 : 0);
  });
}

function buildSlavePanels() {
  const host = $('slave-panels');
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const panel = new SlavePanel(i);
    slavePanels[i] = panel;
    panel.setConnected(false);
    loadSlavePanelState(i, panel);
    wireSlavePanel(i, panel);
    host.appendChild(panel.root);
  }
}

/**
 * After any geo's offset changes, sort all geo-type nodes by distance from the hammer
 * and re-label their panels as Geo1, Geo2, ... (closest first).
 * Slave IDs (S1, S2...) remain static — only the display name changes.
 */
function reorderGeosByOffset() {
  // Collect connected/present geo indices. Hidden placeholders must not steal
  // Geo1/Geo2 labels from real nodes when their default offset is zero.
  const geos = [];
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const nd = data.nodes[i];
    if (nd.alias !== 'Hammer' && nodeHasPresence(i)) {
      geos.push({ idx: i, dist: nd.hammerOffset || 0 });
    }
  }
  // Sort by distance ascending (closest to source = Geo1)
  geos.sort((a, b) => (a.dist - b.dist) || (a.idx - b.idx));
  geos.forEach(({ idx }, rank) => {
    const name = `Geo${rank + 1}`;
    data.nodes[idx].alias = name;
    saveSlaveSetting(idx, 'alias', name);
    const panel = slavePanels[idx];
    if (panel) {
      panel.setAlias(name);
      panel.setDisplayName(`${name} (${cfg.NODE_NAMES[idx]})`);
    }
    renderNodeRow(idx);
  });
  refreshSlavePresentationOrder();
}

function calibrationProgressLabel(v) {
  const stages = ['GEO_PGA', 'GEO_BP', 'GEO_ADDER', 'GEO_LP'];
  if (v >= 3 && v <= 6) return `biseccion ${stages[v - 3]}`;
  if (v >= 7 && v <= 10) return `verify ${stages[v - 7]}`;
  if (v >= 11 && v <= 14) return `realcheck ${stages[v - 11]}`;
  return 'en curso';
}

function handleAck(pkt, idx) {
  const ackCmd = pkt.ackCmd;
  const ackVal = pkt.ackVal;

  // Progreso de calibracion (ok>=2): no consume el pending de la confirmacion
  // final, solo refleja fase/etapa en la UI.
  if (ackCmd === cfg.SUBCMD_CALIBRATE && ackVal >= 2 && idx >= 1 && idx < cfg.MAX_NODES) {
    const nd = data.nodes[idx];
    const panel = panelFor(idx);
    const progress = calibrationProgressLabel(ackVal);
    if (panel) panel.setCalibrationLock(3, progress);
    const now = performance.now();
    if (now - nd.calProgressLogMs >= 15000) {
      nd.calProgressLogMs = now;
      appendLog(`S${idx} calibrando... ${progress}`);
    }
    return;
  }

  let pending = null;
  if (idx >= 0 && idx < cfg.MAX_NODES) {
    const nd = data.nodes[idx];
    pending = nd.pending.get(ackCmd) || null;
    nd.pending.delete(ackCmd);
  }

  if (ackCmd === cfg.SUBCMD_PGA && idx >= 1 && idx < cfg.MAX_NODES) {
    const panel = panelFor(idx);
    if (panel) panel.setPgaLock(ackVal ? 1 : 2);
    if (pgaLockTimers.has(idx)) {
      clearTimeout(pgaLockTimers.get(idx));
      pgaLockTimers.delete(idx);
    }
    const expected = pending ? pending.param : data.nodes[idx].pgaCode;
    appendLog(`S${idx} PGA ${ackVal ? 'confirmado' : 'sin lock'}: ${cfg.GAIN_NAMES[expected] ?? expected}`);
    return;
  }

  if (ackCmd === cfg.SUBCMD_VDAC && idx >= 1 && idx < cfg.MAX_NODES) {
    appendLog(`ACK legacy VDAC S${idx}: ${ackVal}`);
    return;
  }

  if (ackCmd === cfg.SUBCMD_CALIBRATE && idx >= 1 && idx < cfg.MAX_NODES) {
    const panel = panelFor(idx);
    if (panel) panel.setCalibrationLock(ackVal ? 1 : 2);
    appendLog(`S${idx} calibracion ${ackVal ? 'confirmada' : 'fallida'}`);
    return;
  }

  if (ackCmd === cfg.SUBCMD_SAVE_EEPROM && idx >= 1 && idx < cfg.MAX_NODES) {
    const panel = panelFor(idx);
    if (panel) panel.setEepromLock(ackVal ? 1 : 2);
    appendLog(`S${idx} EEPROM ${ackVal ? 'guardada OK' : 'error al guardar'}`);
    return;
  }

  if (ackCmd === cfg.SUBCMD_BLINK_LED && idx >= 1 && idx < cfg.MAX_NODES) {
    appendLog(`S${idx} LED blink ACK`);
    return;
  }

  if (ackCmd === cfg.SUBCMD_SELECT_STREAM && idx >= 1 && idx < cfg.MAX_NODES) {
    appendLog(`S${idx} stream: ${ackVal ? 'FIR hardware' : 'ADC crudo'}`);
    return;
  }

  appendLog(`ACK node=${pkt.nodeId} cmd=0x${ackCmd.toString(16).toUpperCase()} val=${ackVal}`);
}

function handleMacPacket(pkt, chIndex) {
  const parts = macPartial.get(chIndex) || {};
  parts[pkt.helloMacSub] = [pkt.helloMacHi, pkt.helloMacLo];
  macPartial.set(chIndex, parts);
  if (!(0x02 in parts) || !(0x03 in parts) || !(0x04 in parts)) return;

  const macBytes = [
    parts[0x02][0], parts[0x02][1],
    parts[0x03][0], parts[0x03][1],
    parts[0x04][0], parts[0x04][1],
  ];
  const mac = macBytes.map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join(':');
  data.nodes[chIndex].mac = mac;
  const panel = panelFor(chIndex);
  if (panel) panel.setMac(mac);
  saveSlaveSetting(chIndex, 'mac', mac);
  macPartial.delete(chIndex);
  appendLog(`HELLO slave=${chIndex} MAC=${mac}`);
}

function handleStatus(pkt, idx) {
  if (pkt.statusIsMaster) {
    appendLog(`Master status: espnow_ok=${pkt.statusEspnowOk} ch=${pkt.statusApCh}`);
    return;
  }

  if (idx <= 0 || idx >= cfg.MAX_NODES) return;

  if (pkt.helloMacSub === 0x02 || pkt.helloMacSub === 0x03 || pkt.helloMacSub === 0x04) {
    handleMacPacket(pkt, idx);
    return;
  }

  if (pkt.helloMacSub === 0x05) {
    updateNodeExactFsFromHello(idx, pkt.helloFsExactHz);
    updateSlavePanelStats(idx);
    renderNodeRow(idx);
    appendLog(`HELLO slave=${idx} fs_exact=${pkt.helloFsExactHz}Hz`);
    return;
  }

  if (pkt.helloMacSub === 0x06) {
    applyReportedSlaveType(idx, pkt.helloHwClass);
    updateSlavePanelStats(idx);
    renderNodeRow(idx);
    appendLog(`HELLO slave=${idx} tipo=${slaveHwClassName(pkt.helloHwClass)}`);
    return;
  }

  if (pkt.helloMacSub === 0x01) {
    const nd = data.nodes[idx];
    nd.psocOk = pkt.helloPsocOk;
    updateNodeFsFromHello(idx, pkt.helloFsHz);
    updateSlavePanelStats(idx);
    renderNodeRow(idx);
    appendLog(`HELLO slave=${idx} psoc_ok=${pkt.helloPsocOk} fs=${pkt.helloFsHz || '?'}Hz`);
  }
}

function preparePreservedOverlay(capture) {
  const removeDc = displayDcRemoveEnabled();
  const yOffsetV = captureYOffsetMv(capture) / 1000;
  return {
    ...capture,
    nodes: (capture.nodes || []).map((node) => {
      if (!node) return node;
      const invert = !!node.invert_signal;
      return {
        ...node,
        raw: transformSignalForView(node.raw, { removeDc, invert, yOffsetV }),
        filt: transformSignalForView(node.filt, { removeDc, invert, yOffsetV }),
      };
    }),
  };
}

function renderTick() {
  const fs = displayFsHz();
  const removeDc = displayDcRemoveEnabled();
  const rawBufsOrig = data.nodes.map((nd) => (nd.rawBuf.length ? nd.rawBuf.toArray() : null));
  const filtBufs = data.nodes.map((nd, i) => (rawBufsOrig[i] ? filteredArrayForNode(nd, rawBufsOrig[i]) : null));
  // filtFilt is zero-phase — no group-delay trim needed.
  const filtTrims = data.nodes.map(() => 0);
  updateWaveVelocityLabel(fs);
  const overlays = selectedPreservedCaptures().map(preparePreservedOverlay);

  // Apply display DC removal and signal inversion to copies only.
  const rawBufsDisplay = rawBufsOrig.map((orig, i) => {
    const nd = data.nodes[i];
    return transformSignalForView(orig, { removeDc, invert: nd.invertSignal });
  });
  const filtBufsDisplay = filtBufs.map((filt, i) => {
    const nd = data.nodes[i];
    return transformSignalForView(filt, { removeDc, invert: nd.invertSignal });
  });

  plotArea.update(rawBufsDisplay, filtBufsDisplay, fs, filtTrims, overlays);
  if (!$('spectra').hidden) spectrumArea.update(rawBufsDisplay, fs, overlays);

  updateGlobalFsDisplay();
  for (const i of orderedSlaveIndices(true)) {
    renderNodeRow(i);
    updateSlavePanelStats(i);
  }
}

function spectrumActive() {
  const el = $('spectra');
  return !!el && !el.hidden;
}

function zoomPlotControls(factor, anchor = 0.5) {
  plotArea.zoomBy(factor, anchor);
  if (spectrumActive()) spectrumArea.zoomBy(factor, anchor);
}

function resetPlotControls() {
  plotArea.resetView();
  spectrumArea.resetView();
}

function applyTheme(light) {
  document.body.classList.toggle('light', light);
  $('btn-theme').textContent = light ? 'Modo oscuro' : 'Modo claro';
}

function initTheme() {
  const saved = loadSetting(`${SETTINGS_PREFIX}theme`);
  applyTheme(saved === 'light');
}

function initTabs() {
  const tabs = Array.from(document.querySelectorAll('.tab-btn[data-tab-target]'));
  const pages = Array.from(document.querySelectorAll('.tab-page'));
  const activate = (targetId) => {
    for (const tab of tabs) {
      const active = tab.dataset.tabTarget === targetId;
      tab.classList.toggle('active', active);
      tab.setAttribute('aria-selected', active ? 'true' : 'false');
    }
    for (const page of pages) page.classList.toggle('active', page.id === targetId);
  };
  for (const tab of tabs) tab.addEventListener('click', () => activate(tab.dataset.tabTarget));
}

$('btn-theme').addEventListener('click', () => {
  const light = !document.body.classList.contains('light');
  applyTheme(light);
  try {
    localStorage.setItem(`${SETTINGS_PREFIX}theme`, light ? 'light' : 'dark');
  } catch (_) {
    // localStorage can be disabled on some phone browser/privacy modes.
  }
});

initTheme();
initTabs();
buildNodeRows();
buildSlavePanels();
renderPreservedList();
restoreFsFromLocalCache();
setConnIndicator(false);
setMasterState(masterState);
setActiveSlaveCount(activeSlaveCount);
updateGlobalFsDisplay();
setInterval(renderTick, cfg.RENDER_PERIOD_MS);

// Cargar token guardado y conectar
const _savedToken = loadSetting(WS_TOKEN_KEY);
if (_savedToken) applyAuthToken(_savedToken);

ws.addEventListener('auth-required', (ev) => {
  const wrongPassword = ev.detail?.reason === 'wrong_password';
  showAuthModal(wrongPassword);
});

$('btn-auth-connect').addEventListener('click', async () => {
  const token = $('auth-password')?.value?.trim() || '';
  applyAuthToken(token);
  hideAuthModal();
  ws.stop();
  await resetWsLock();   // libera el WS lock del servidor antes de reconectar
  ws.start();
});

$('auth-password').addEventListener('keydown', (ev) => {
  if (ev.key === 'Enter') $('btn-auth-connect').click();
});

ws.addEventListener('connection', (ev) => {
  if (ev.detail) hideAuthModal();
  setConnIndicator(ev.detail);
  if (ev.detail) {
    // Ask for cached READY/HELLO immediately and once more if Fs is still
    // missing. This keeps Fs hardware-sourced, without requiring manual input
    // after a browser reconnect or master reboot.
    sendStd(cfg.CMD_STATUS, 0);
    setTimeout(() => {
      if (ws.connected && !currentFsHz()) sendStd(cfg.CMD_STATUS, 0);
    }, 800);
    setTimeout(() => {
      if (ws.connected && !currentFsHz()) sendStd(cfg.CMD_STATUS, 0);
    }, 2500);
  }
});

ws.addEventListener('log', (ev) => appendLog(ev.detail));

ws.addEventListener('packet', (ev) => {
  const pkt = ev.detail;
  const idx = pkt.nodeId === cfg.MASTER_NODE_ID ? 0 : pkt.nodeId;
  if (idx < 0 || idx >= cfg.MAX_NODES) return;
  const nd = data.nodes[idx];

  if (pkt.isData) {
    handleData(nd, pkt);
  } else if (pkt.isHeartbeat) {
    nd.pgaCode = pkt.hbPga;
    nd.vdacByte = pkt.hbVdac;
    if (idx === 0) {
      masterState = pkt.hbMasterState;
      setMasterState(masterState);
    } else {
      const panel = panelFor(idx);
      if (panel) {
        panel.setPga(nd.pgaCode);
      }
    }
  } else if (pkt.isAck) {
    handleAck(pkt, idx);
  } else if (pkt.isReady) {
    setActiveSlaveCount(pkt.readyNSlaves);
    appendLog(`READY n_slaves=${pkt.readyNSlaves}`);
  } else if (pkt.isStatus) {
    handleStatus(pkt, idx);
  } else if (pkt.isLatency) {
    nd.addLatency(pkt.value24u);
    updateSlavePanelStats(idx);
    appendLog(`LATENCY node=${pkt.nodeId} ${pkt.value24u} us`);
  }
  if (idx > 0) ensureSlaveVisible(idx);
});

// Controls

$('btn-connect').addEventListener('click', async () => {
  ws.stop();
  await resetWsLock();
  ws.start();
});

$('btn-arm').addEventListener('click', () => {
  const n = clamp(parseInt($('arm-n').value, 10) || 0, 0, cfg.MAX_NODES - 1);
  setActiveSlaveCount(n, true);
  sendStd(cfg.CMD_ARM, n);
});

// Manual override for when no slave's HELLO carries Fs (e.g. master rebooted
// while slaves were already armed — HELLO is only beaconed from WAIT_ARM).
// All slaves share the same hardware Fs, so one operator-entered value covers
// every node, exactly like a real HELLO would via applyGlobalFs().
$('btn-manual-fs').addEventListener('click', () => {
  const fsHz = parseFloat($('manual-fs-hz').value);
  if (!(fsHz > 0)) {
    appendLog('Fs manual invalida: ingresa un valor en Hz > 0');
    return;
  }
  saveGlobalFs(fsHz);
  applyGlobalFs(fsHz, `Fs aplicada manualmente: ${fsHz} Hz`);
});

$('btn-start').addEventListener('click', () => {
  if (!currentFsHz()) {
    appendLog('START cancelado: esperando Fs real del esclavo (HELLO no recibido aun)');
    return;
  }
  const requestedSecs = captureSeconds();
  const info = captureLimitInfo(requestedSecs);
  const n = info.n;
  data.clearAll();
  plotArea.clearAll();
  spectrumArea.clearAll();
  sendStd16(cfg.CMD_START, n);
  const capped = info.capped ? `, max; pedido ${requestedSecs.toFixed(2)} s` : '';
  appendLog(`START: ${n} lotes (~${info.actualSecs.toFixed(2)} s real${capped})`);
});

$('btn-preserve').addEventListener('click', onPreserveRequested);

$('btn-stop').addEventListener('click', () => {
  sendStd(cfg.CMD_STOP, 0);
  sendStd(cfg.CMD_STREAM, 0);
});

$('btn-status').addEventListener('click', () => {
  sendStd(cfg.CMD_STATUS, 0);
});

$('btn-cal-all').addEventListener('click', () => {
  if (!ws.connected) return;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].psocOk !== null) onCalibrateRequested(i);
  }
  appendLog('Calibracion solicitada a todos los esclavos');
});

$('btn-export').addEventListener('click', onExportRequested);
$('btn-preserved-all').addEventListener('click', () => setAllPreservedVisible(true));
$('btn-preserved-none').addEventListener('click', () => setAllPreservedVisible(false));
$('btn-preserved-clear').addEventListener('click', clearPreservedCaptures);

$('btn-plot-zoom-in').addEventListener('click', () => zoomPlotControls(1.5, 0.5));
$('btn-plot-zoom-out').addEventListener('click', () => zoomPlotControls(1 / 1.5, 0.5));
$('btn-plot-reset').addEventListener('click', resetPlotControls);

function setCursorEditMode(mode) {
  const buttons = [$('btn-cursor-1'), $('btn-cursor-2')];
  const wasActive = buttons[mode].classList.contains('active');
  for (const btn of buttons) btn.classList.remove('active');
  plotArea.setCursorMode(wasActive ? null : mode);
  if (!wasActive) buttons[mode].classList.add('active');
}

$('btn-cursor-1').addEventListener('click', () => setCursorEditMode(0));
$('btn-cursor-2').addEventListener('click', () => setCursorEditMode(1));

$('chk-spectrum').addEventListener('change', () => {
  const active = $('chk-spectrum').checked;
  $('plots').hidden = active;
  $('spectra').hidden = !active;
  if (active) spectrumArea.resetView();
});

// Global DC removal (applies to all nodes, raw AND filtered).
$('chk-dc-remove').addEventListener('change', () => {
  const enabled = $('chk-dc-remove').checked;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    data.nodes[i].dcRemove = enabled;
    reprocessFiltBuf(i);
  }
});

// Display-only curve visibility — saving/export always keeps raw AND filtered regardless of these.
function applyCurveVisibility(persist = true) {
  const showRaw = $('chk-show-raw').checked;
  const showFilt = $('chk-show-filt').checked;
  const showEnvRaw = $('chk-show-env-raw').checked;
  const showEnvFilt = $('chk-show-env-filt').checked;
  if (persist) {
    saveSetting(SHOW_RAW_KEY, showRaw ? 1 : 0);
    saveSetting(SHOW_FILT_KEY, showFilt ? 1 : 0);
    saveSetting(SHOW_ENV_RAW_KEY, showEnvRaw ? 1 : 0);
    saveSetting(SHOW_ENV_FILT_KEY, showEnvFilt ? 1 : 0);
  }
  plotArea.setCurveVisibility(showRaw, showFilt);
  plotArea.setEnvelopeMode(showEnvRaw, showEnvFilt);
}

function initCurveVisibility() {
  $('chk-show-raw').checked = loadBoolSetting(SHOW_RAW_KEY, $('chk-show-raw').checked);
  $('chk-show-filt').checked = loadBoolSetting(SHOW_FILT_KEY, $('chk-show-filt').checked);
  const oldEnvelope = loadBoolSetting(SHOW_HILBERT_KEY, false);
  $('chk-show-env-raw').checked = loadBoolSetting(SHOW_ENV_RAW_KEY, oldEnvelope);
  $('chk-show-env-filt').checked = loadBoolSetting(SHOW_ENV_FILT_KEY, oldEnvelope);
  applyCurveVisibility(false);
}
$('chk-show-raw').addEventListener('change', applyCurveVisibility);
$('chk-show-filt').addEventListener('change', applyCurveVisibility);
$('chk-show-env-raw').addEventListener('change', applyCurveVisibility);
$('chk-show-env-filt').addEventListener('change', applyCurveVisibility);
initCurveVisibility();

function onCaptureDurationEdited() {
  updateCapturePreview();
  syncDisplayWindowToCaptureDuration();
}

$('disp-secs').addEventListener('input', applyDisplayWindow);
$('disp-secs').addEventListener('change', applyDisplayWindow);
$('capture-secs').addEventListener('input', onCaptureDurationEdited);
$('capture-secs').addEventListener('change', onCaptureDurationEdited);
syncDisplayWindowToCaptureDuration();

ws.start();
