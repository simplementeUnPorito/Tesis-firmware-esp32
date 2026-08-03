// app.js - bootstraps the web UI. Mirrors the orchestration role of
// gui/main_window.py: WebSocket packets -> DataStore/UI, and UI actions ->
// the same command bytes that handleMatlabCmd() already consumes.

import * as cfg from './config.js?v=field-study-19';
import { WsClient } from './ws_client.js?v=field-study-19';
import { encodeStd, encodeStd16, encodeDirected } from './protocol.js?v=field-study-19';
import { DataStore, effectiveFs } from './data_store.js?v=field-study-19';
import { PlotArea } from './plot.js?v=field-study-19';
import { SpectrumArea } from './spectrum.js?v=field-study-19';
import { SlavePanel } from './slave_panel.js?v=field-study-19';
import { compileFirCmd, dcRemove, filtFilt, harmonicNotch, hilbertEnvelope, lastFirError } from './signal_proc.js?v=field-study-19';
import { initEnlaceTab, getServerUrl } from './enlace.js?v=field-study-19';
import { buildCaptureZip, downloadBlob } from './export.js?v=field-study-19';

const $ = (id) => document.getElementById(id);

const ws = new WsClient(`ws://${location.host}/ws`);
const data = new DataStore();
const plotArea = new PlotArea($('plots'));
const spectrumArea = new SpectrumArea($('spectra'));
const slavePanels = new Array(cfg.MAX_NODES).fill(null);
const macPartial = new Map();
const pgaLockTimers = new Map();
const pgaoutLockTimers = new Map();
const adcConfigLockTimers = new Map();
const decimationLockTimers = new Map();
const sdEnableLockTimers = new Map();
const testTimers = new Map();
const preservedCaptures = [];
// Firmas de capturas ya incluidas en un ZIP exportado con exito — usado por
// el guard de beforeunload para avisar solo si hay datos NUEVOS sin bajar
// (no cada vez que hay algo en preservedCaptures, que se mantiene tras exportar).
const exportedCaptureSignatures = new Set();
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
let currentCaptureSignature = '';
let pendingStart = null;

const START_AUTO_ARM_TIMEOUT_MS = 4500;

const SETTINGS_PREFIX = 'geophone_scope_web.';
// Versionar la Fs evita restaurar valores derivados de la antigua base
// 2929/1020. El factor N se guarda por separado: es la preferencia real del
// operador y sigue siendo válido aunque cambie la Fs nativa del firmware.
const GLOBAL_FS_KEY = `${SETTINGS_PREFIX}fs_hz_v4`;
const GLOBAL_DECIMATION_KEY = `${SETTINGS_PREFIX}decimation_factor_v1`;
const SHOW_RAW_KEY = `${SETTINGS_PREFIX}show_raw`;
const SHOW_FILT_KEY = `${SETTINGS_PREFIX}show_filt`;
const SHOW_ENV_RAW_KEY = `${SETTINGS_PREFIX}show_env_raw`;
const SHOW_ENV_FILT_KEY = `${SETTINGS_PREFIX}show_env_filt`;
const SHOW_HILBERT_KEY = `${SETTINGS_PREFIX}show_hilbert`;
const DISPLAY_DC_REMOVE_KEY = `${SETTINGS_PREFIX}display_dc_remove`;
const WS_TOKEN_KEY  = `${SETTINGS_PREFIX}ws_token`;

const CURVE_SOURCE_DEFS = [
  { key: 'raw', checkboxId: 'chk-show-raw', label: 'Cruda', color: cfg.RAW_COLOR },
  { key: 'filt', checkboxId: 'chk-show-filt', label: 'Filtrada', color: cfg.FILT_COLOR },
  { key: 'envRaw', checkboxId: 'chk-show-env-raw', label: 'Env cruda', color: cfg.ENV_RAW_COLOR },
  { key: 'envFilt', checkboxId: 'chk-show-env-filt', label: 'Env filtrada', color: cfg.ENV_FILT_COLOR },
];

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

function clearSlaveSetting(chIndex, name) {
  try {
    localStorage.removeItem(settingKey(chIndex, name));
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

function saveGlobalDecimation(factor) {
  const n = clamp(parseInt(factor, 10) || 1, 1, 100);
  saveSetting(GLOBAL_DECIMATION_KEY, n);
  return n;
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

function adcConfigForCode(code) {
  return cfg.ADC_CONFIGS.find((item) => item.code === code) || cfg.ADC_CONFIGS[0];
}

function applyNodeAdcConfig(nd, code) {
  const adcCfg = adcConfigForCode(code);
  nd.adcConfigCode = adcCfg.code;
  nd.countsPerVolt = adcCfg.countsPerVolt;
  return adcCfg;
}

function applyNodeDecimation(nd, factor) {
  const f = clamp(parseInt(factor, 10) || 1, 1, 100);
  nd.decimationFactor = f;
  return f;
}

function applyNodeSdEnabled(nd, enabled) {
  const e = !!enabled;
  nd.sdEnabled = e;
  return e;
}

function emptyCalVdacs() {
  return Array.from({ length: cfg.CAL_VDAC_STAGE_COUNT }, () => null);
}

function emptyCalVdacDetails() {
  return Array.from({ length: cfg.CAL_VDAC_STAGE_COUNT }, () => null);
}

function vdacCodeMv(code) {
  return Number.isFinite(code) ? Math.round(code * cfg.VDAC_MV_PER_CODE) : null;
}

function signed16FromBytes(hi, lo) {
  const raw = ((hi & 0xFF) << 8) | (lo & 0xFF);
  return raw >= 0x8000 ? raw - 0x10000 : raw;
}

function cloneCalVdacDetails(details) {
  if (!Array.isArray(details)) return emptyCalVdacDetails();
  return details.map((item) => item ? { ...item } : null);
}

function ensureCalVdacDetail(nd, stage) {
  if (!Array.isArray(nd.calVdacDetails)) nd.calVdacDetails = emptyCalVdacDetails();
  if (!nd.calVdacDetails[stage]) nd.calVdacDetails[stage] = { stage };
  return nd.calVdacDetails[stage];
}

function updateCalVdacComputed(detail) {
  if (!detail) return;
  detail.code_mV = vdacCodeMv(detail.code);
  const target = Number(detail.target_mV);
  const error = Number(detail.error_mV);
  if (Number.isFinite(target) && target !== 0 && Number.isFinite(error)) {
    detail.error_pct = (error / Math.abs(target)) * 100;
    detail.error_abs_pct = Math.abs(error) / Math.abs(target) * 100;
  } else {
    detail.error_pct = null;
    detail.error_abs_pct = null;
  }
}

function setCalVdacCode(nd, stage, code) {
  if (!Array.isArray(nd.calVdacs)) nd.calVdacs = emptyCalVdacs();
  nd.calVdacs[stage] = code;
  const detail = ensureCalVdacDetail(nd, stage);
  detail.code = code;
  updateCalVdacComputed(detail);
}

function setCalVdacSignedWord(nd, stage, field, part, byteValue) {
  const detail = ensureCalVdacDetail(nd, stage);
  detail[`_${field}_${part}`] = byteValue & 0xFF;
  if (Number.isFinite(detail[`_${field}_hi`]) && Number.isFinite(detail[`_${field}_lo`])) {
    detail[field] = signed16FromBytes(detail[`_${field}_hi`], detail[`_${field}_lo`]);
    delete detail[`_${field}_hi`];
    delete detail[`_${field}_lo`];
    updateCalVdacComputed(detail);
  }
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

function clampLineNotchHarm(value) {
  const v = parseInt(value, 10);
  if (!Number.isFinite(v)) return cfg.LINE_NOTCH_DEFAULT_HARM;
  return Math.min(cfg.LINE_NOTCH_MAX_HARM, Math.max(1, v));
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

function envelopeForSpectrum(src) {
  if (!src || src.length < 2) return null;
  let mean = 0;
  for (let i = 0; i < src.length; i++) mean += src[i];
  mean /= src.length;
  const centered = new Float64Array(src.length);
  for (let i = 0; i < src.length; i++) centered[i] = src[i] - mean;
  return hilbertEnvelope(centered);
}

function selectedCurveSourceDefs() {
  return CURVE_SOURCE_DEFS.filter((def) => {
    const cb = $(def.checkboxId);
    return cb && cb.checked;
  });
}

function buildSpectrumSources(raw, filt) {
  const out = [];
  for (const def of selectedCurveSourceDefs()) {
    let data = null;
    if (def.key === 'raw') data = raw;
    else if (def.key === 'filt') data = filt;
    else if (def.key === 'envRaw') data = envelopeForSpectrum(raw);
    else if (def.key === 'envFilt') data = envelopeForSpectrum(filt);
    if (data && data.length) out.push({ key: def.key, label: def.label, color: def.color, data });
  }
  return out;
}

function nodeConnectedForCapture(nd, index) {
  return index > 0
    && (nd.visible || nd.rawBuf.length > 0 || nd.filtBuf.length > 0)
    && (nd.fsKnown || !!nd.mac || nd.hwClass !== 0xFF || nd.rawBuf.length > 0 || nd.filtBuf.length > 0);
}

function nodeRole(nd) {
  if (!nd) return 'unknown';
  if (nd.hwClass === 1 || nd.alias === 'Hammer') return 'hammer';
  if (nd.hwClass === 0 || String(nd.alias || '').startsWith('Geo')) return 'geo';
  return 'unknown';
}

function geoNumberFromAlias(alias) {
  const m = /^Geo(\d+)$/i.exec(String(alias || '').trim());
  return m ? parseInt(m[1], 10) : null;
}

function offsetFromHammer(nd) {
  return nodeRole(nd) === 'hammer' ? 0 : (nd.hammerOffset ?? 0);
}

function nodeSnapshot(nd, index, options = {}) {
  const rawOrig = nd.rawBuf.toArray();
  const filtOrig = filteredArrayForSnapshot(index, rawOrig);
  const storeDcRemoved = !!options.storeDcRemoved;
  const rawDc = signalMean(rawOrig);
  const filtDc = signalMean(filtOrig);
  const raw = storeDcRemoved ? transformSignalForView(rawOrig, { removeDc: true }) : rawOrig;
  const filt = storeDcRemoved ? transformSignalForView(filtOrig, { removeDc: true }) : filtOrig;
  const role = nodeRole(nd);
  const offsetM = offsetFromHammer(nd);
  const pcbId = nd.slaveId || (index === 0 ? 'M' : `S${index}`);
  return {
    index,
    node_id: index,
    pcb_id: pcbId,
    physical_id: pcbId,
    name: cfg.NODE_NAMES[index] || `Node ${index}`,
    type: nd.alias || cfg.NODE_NAMES[index] || `Node ${index}`,
    role,
    geo_number: role === 'geo' ? geoNumberFromAlias(nd.alias) : null,
    slave_id: pcbId,
    fs: nd.fs,
    fs_known: !!nd.fsKnown,
    fs_exact_known: !!nd.fsExactKnown,
    connected: nodeConnectedForCapture(nd, index),
    raw,
    filt,
    raw_count: raw.length,
    filt_count: filt.length,
    batch_count: nd.batchCount,
    total_samples: nd.totalSamples,
    pga_code: nd.pgaCode,
    pga_gain: gainValue(nd.pgaCode),
    pgaout_code: nd.pgaoutCode,
    pgaout_gain: gainValue(nd.pgaoutCode),
    adc_config: nd.adcConfigCode,
    adc_config_label: adcConfigForCode(nd.adcConfigCode).label,
    adc_range_v: adcConfigForCode(nd.adcConfigCode).rangeV,
    adc_counts_per_volt: nd.countsPerVolt || cfg.ADC_COUNTS_PER_VOLT,
    decimation_factor: nd.decimationFactor,
    vdac_byte: nd.vdacByte,
    cal_vdacs: Array.isArray(nd.calVdacs) ? nd.calVdacs.slice() : [],
    cal_vdac_details: cloneCalVdacDetails(nd.calVdacDetails),
    pgavdac_code: nd.pgavdac,
    pgavdac_gain: gainValue(nd.pgavdac),
    psoc_ok: nd.psocOk,
    hw_class: nd.hwClass,
    hw_type: slaveHwClassName(nd.hwClass),
    hammer_offset_m: offsetM,
    offset_m_from_hammer: offsetM,
    position_m: offsetM,
    sample_offset: 0,
    mac: nd.mac || '',
    visible: !!nd.visible,
    fir_cmd: nd.filtCmd || '',
    filt_trim_samples: 0,
    dc_remove: !!nd.dcRemove,
    dc_removed_on_preserve: storeDcRemoved,
    notch_enabled: !!nd.notchEnabled,
    notch_harm: nd.notchHarm | 0,
    raw_dc_v: rawDc,
    filt_dc_v: filtDc,
    invert_signal: !!nd.invertSignal,
    display_y_offset_v: nd.yOffsetV || 0,
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

/* true si hay una captura preservada o el buffer en vivo con muestras cuya
 * firma todavia no forma parte de un ZIP exportado con exito. Se recalcula
 * al vuelo (no es un flag manual) para no desincronizarse de preservedCaptures. */
function hasUnexportedData() {
  for (const capture of preservedCaptures) {
    if (!exportedCaptureSignatures.has(capture.signature)) return true;
  }
  const liveCapture = makeCaptureSnapshot('Actual', { source: 'live', visible: false });
  return captureHasSamples(liveCapture) && !exportedCaptureSignatures.has(liveCapture.signature);
}

function findPreservedCaptureIndexBySignature(signature) {
  if (!signature) return -1;
  return preservedCaptures.findIndex((capture) => capture.signature === signature);
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

function captureXOffsetStep(capture) {
  const fs = Number(capture?.fs) || displayFsHz();
  return Math.max(1, Math.round((fs || cfg.DEFAULT_SAMPLE_RATE_HZ) / 10));
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
    notch_f0: cfg.LINE_NOTCH_F0,
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
    sub.textContent = `${capture.display_time} · ${capture.raw_count} raw · Fs ${capture.fs ? capture.fs.toFixed(0) : '?'} Hz · ${dcText}`;

    const offsetWrap = document.createElement('label');
    offsetWrap.className = 'preserve-offset';
    offsetWrap.appendChild(document.createTextNode('Offset X '));
    const offsetInput = document.createElement('input');
    offsetInput.type = 'number';
    offsetInput.step = String(captureXOffsetStep(capture));
    offsetInput.value = String(captureSampleOffset(capture));
    offsetInput.title = 'Mueve esta captura en el eje X, en muestras. Acepta valores positivos o negativos.';
    const applyXOffset = (rerender = false) => {
      const parsed = parseInt(offsetInput.value, 10);
      capture.sample_offset = Number.isFinite(parsed) ? parsed : 0;
      if (rerender) renderPreservedList();
      refreshSlavePresentationOrder();
    };
    offsetInput.addEventListener('input', () => applyXOffset(false));
    offsetInput.addEventListener('change', () => applyXOffset(true));
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

    meta.append(title, sub, offsetWrap, yOffsetWrap);

    const del = document.createElement('button');
    del.type = 'button';
    del.textContent = 'Quitar';
    del.addEventListener('click', () => {
      const idx = preservedCaptures.findIndex((item) => item.id === capture.id);
      if (idx >= 0) preservedCaptures.splice(idx, 1);
      renderPreservedList();
      refreshSlavePresentationOrder();
    });

    row.append(chk, swatch, meta, del);
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
  currentCaptureSignature = '';
  renderPreservedList();
  refreshSlavePresentationOrder();
}

function clearLiveCapture() {
  data.clearAll();
  plotArea.clearAll();
  spectrumArea.clearAll();
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    renderNodeRow(i);
    updateSlavePanelStats(i);
  }
}

function preserveCurrentCapture(reason = 'captura completa') {
  const nextOrder = preserveSeq + 1;
  const capture = makeCaptureSnapshot(`Start ${nextOrder}`, {
    source: 'preserved',
    order: nextOrder,
    visible: true,
    storeDcRemoved: displayDcRemoveEnabled(),
  });
  if (!captureHasSamples(capture)) {
    currentCaptureSignature = '';
    return null;
  }
  const duplicateIdx = findPreservedCaptureIndexBySignature(capture.signature);
  if (duplicateIdx >= 0) {
    currentCaptureSignature = capture.signature;
    return preservedCaptures[duplicateIdx];
  }

  preserveSeq = nextOrder;
  currentCaptureSignature = capture.signature;
  preservedCaptures.push(capture);
  renderPreservedList();
  refreshSlavePresentationOrder();
  appendLog(`Guardado automatico ${capture.label} (${reason}): ${capture.raw_count} muestras raw`);
  return capture;
}

function onDiscountRequested() {
  const liveCapture = makeCaptureSnapshot('Actual', {
    source: 'live',
    visible: false,
    storeDcRemoved: displayDcRemoveEnabled(),
  });
  const liveHasSamples = captureHasSamples(liveCapture);
  const signature = currentCaptureSignature || (liveHasSamples ? liveCapture.signature : '');
  const idx = findPreservedCaptureIndexBySignature(signature);

  if (idx >= 0) {
    const [removed] = preservedCaptures.splice(idx, 1);
    appendLog(`Descontado ${removed.label}: muestra borrada`);
  } else if (liveHasSamples) {
    appendLog('Descontado: buffer actual borrado');
  } else {
    appendLog('Descontar: no hay muestra actual para borrar');
    return;
  }

  currentCaptureSignature = '';
  clearLiveCapture();
  renderPreservedList();
  refreshSlavePresentationOrder();
}

function captureSeconds() {
  const el = $('capture-secs');
  const parsed = el ? parseFloat(el.value) : NaN;
  const secs = Number.isFinite(parsed) ? parsed : 3.0;
  return clamp(secs, 0.1, cfg.MAX_CAPTURE_SECONDS);
}

/* Duración propia de los nodos HAMMER. null = campo vacío/0 ⇒ usar la misma
 * que los GEOs (el maestro recibe 0xAD=0 y vuelve al largo único legado). */
function hammerCaptureSeconds() {
  const el = $('capture-secs-hammer');
  const parsed = el ? parseFloat(el.value) : NaN;
  if (!Number.isFinite(parsed) || parsed <= 0) return null;
  return clamp(parsed, 0.1, cfg.MAX_CAPTURE_SECONDS);
}

function hammerCaptureBatches() {
  const secs = hammerCaptureSeconds();
  if (secs === null) {
    const geoBatches = captureBatches();
    return geoBatches > cfg.PSOC_RAM_CAPTURE_MAX_BATCHES
      ? cfg.PSOC_RAM_CAPTURE_MAX_BATCHES : 0;
  }
  return Math.min(batchesForSeconds(secs), cfg.PSOC_RAM_CAPTURE_MAX_BATCHES);
}

/* Manda al maestro el largo HAMMER (0xAD) antes de cualquier RECLEN/START.
 * Con el campo vacío manda 0 = "mismo largo que GEO" (camino legado). */
function sendHammerRecLen() {
  sendStd16(cfg.CMD_SET_RECLEN_HAMMER, hammerCaptureBatches());
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
  const hSecs = hammerCaptureSeconds();
  let hammerTxt = '';
  if (hSecs !== null) {
    const hInfo = captureLimitInfo(hSecs);
    hammerTxt = ` · HAMMER ${hInfo.n} lotes / ${hInfo.actualSecs.toFixed(2)} s`;
  }
  el.textContent = `GEO ${info.n} lotes / real ${info.actualSecs.toFixed(2)} s${capped}${hammerTxt}`;
}

/* W1 (auditoría 2026-07-11): fija Duración GEOs al techo de RAM (512 lotes)
 * — capturas sin SD o cuando se quiere evitar el dump largo de la SD. */
function setCaptureToMaxRamBatches() {
  const fs = currentFsHz();
  if (!fs) {
    appendLog('Max RAM cancelado: esperando Fs del esclavo');
    return;
  }
  const secs = (cfg.PSOC_RAM_CAPTURE_MAX_BATCHES * cfg.SAMPLES_PER_BATCH) / fs;
  const el = $('capture-secs');
  // floor: con ceil el redondeo (5.90 s) recomputaba 513 lotes y el esclavo
  // clampaba a 512 — cosmético pero confuso (W6, visto en E10).
  if (el) el.value = (Math.floor(secs * 100) / 100).toFixed(2);
  updateCapturePreview();
  syncDisplayWindowToCaptureDuration();
  appendLog(`Duracion fijada al maximo RAM: ${cfg.PSOC_RAM_CAPTURE_MAX_BATCHES} lotes (~${secs.toFixed(2)} s)`);
}

function setCaptureToMaxBatches() {
  const fs = currentFsHz();
  if (!fs) {
    appendLog('Max paquetes cancelado: esperando Fs del esclavo');
    return;
  }
  const secs = (cfg.PSOC_CAPTURE_MAX_BATCHES * cfg.SAMPLES_PER_BATCH) / fs;
  const roundedUp = Math.ceil(secs * 100) / 100;
  const el = $('capture-secs');
  if (el) el.value = roundedUp.toFixed(2);
  updateCapturePreview();
  syncDisplayWindowToCaptureDuration();
  appendLog(`Duracion fijada al maximo: ${cfg.PSOC_CAPTURE_MAX_BATCHES} lotes (~${secs.toFixed(2)} s)`);
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
  const name = masterStateName(stateCode);
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

  /* PGAout solo existe en el pipeline GEO de la placa nueva. */
  if (panel) panel.setPgaoutVisible(hwClass === 0);

  if (hwClass === 1) {
    nd.alias = 'Hammer';
    nd.hammerOffset = 0;
    saveSlaveSetting(chIndex, 'hammer_offset_m', 0);
    if (panel) {
      panel.setAlias('Hammer');
      panel.setOffset(0);
      panel.setDisplayName(`Hammer (${cfg.NODE_NAMES[chIndex]})`);
    }
    reorderGeosByOffset();
  } else {
    if (nd.alias === 'Hammer' || !String(nd.alias || '').startsWith('Geo')) nd.alias = 'Geo';
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

function masterStateName(stateCode = masterState) {
  return cfg.MASTER_STATE_NAMES[stateCode] ?? `(${stateCode})`;
}

function masterBusyForStart() {
  return [
    cfg.MASTER_STATE_RUNNING,
    cfg.MASTER_STATE_STOPPING,
    cfg.MASTER_STATE_DUMPING,
    cfg.MASTER_STATE_PRESTART,
    cfg.MASTER_STATE_SCOPE_MULTI,
  ].includes(masterState);
}

function clearPendingStart() {
  if (pendingStart?.timer) clearTimeout(pendingStart.timer);
  pendingStart = null;
}

function makeStartRequestInfo() {
  const requestedSecs = captureSeconds();
  const info = captureLimitInfo(requestedSecs);
  return { requestedSecs, info, n: info.n };
}

// Aviso de recorte: pedir mas de 512 lotes sin la SD del PSoC activa hace que
// el esclavo recorte en silencio a 512 (limite RAM conjunto ESP+PSoC). Avisar
// ANTES de capturar para que el usuario active SD o baje la duracion.
function warnIfRamClamp(n, chIndex) {
  if (n <= cfg.PSOC_RAM_CAPTURE_MAX_BATCHES) return;
  const nd = data.nodes[chIndex];
  if (nd && nd.sdPresent && nd.sdEnabled) return;
  appendLog(`AVISO S${chIndex}: ${n} lotes supera la RAM (${cfg.PSOC_RAM_CAPTURE_MAX_BATCHES}); ` +
            'sin "SD" activa en el panel del nodo la captura se recorta a ' +
            `${cfg.PSOC_RAM_CAPTURE_MAX_BATCHES} lotes`);
}

function runStartCapture(req, source = '') {
  clearPendingStart();
  currentCaptureSignature = '';
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (nodeConnectedForCapture(data.nodes[i], i)) warnIfRamClamp(req.n, i);
  }
  resizePresentCaptureBuffers(req.n);
  data.clearAll();
  plotArea.clearAll();
  spectrumArea.clearAll();
  sendHammerRecLen();
  sendStd16(cfg.CMD_START, req.n);
  const capped = req.info.capped ? `, max; pedido ${req.requestedSecs.toFixed(2)} s` : '';
  const suffix = source ? ` (${source})` : '';
  const nHammer = hammerCaptureBatches();
  const hammerTxt = nHammer ? `, HAMMER ${nHammer} lotes` : '';
  appendLog(`START${suffix}: ${req.n} lotes GEO (~${req.info.actualSecs.toFixed(2)} s real${capped})${hammerTxt}`);
}

function queueStartAfterArm(req, armCount) {
  clearPendingStart();
  pendingStart = {
    req,
    timer: setTimeout(() => {
      pendingStart = null;
      appendLog('START cancelado: ARM no devolvio READY; revisa enlace/esclavo');
    }, START_AUTO_ARM_TIMEOUT_MS),
  };
  sendStd(cfg.CMD_ARM, armCount);
  appendLog(`START: READY n=0, ARM automatico n=${armCount}; esperando READY`);
}

function maybeRunPendingStart(readyCount) {
  if (!pendingStart) return;
  if (masterBusyForStart()) {
    appendLog(`START pendiente bloqueado: master ${masterStateName()}, envia STOP antes de otro START`);
    clearPendingStart();
    return;
  }
  if (readyCount > 0) runStartCapture(pendingStart.req, 'post-ARM');
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

function resizePresentCaptureBuffers(nBatches, preferredIndex = 0) {
  const fs = currentFsHz();
  const base = Math.max(cfg.SAMPLES_PER_BATCH, Math.round(cfg.MAX_BUF_S * (fs || 1)));
  const requested = Math.max(base, nBatches * cfg.SAMPLES_PER_BATCH);
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const active = i === preferredIndex || nodeHasPresence(i);
    data.nodes[i].resizeBuf(active ? requested : base);
  }
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
    ` adc=${adcConfigForCode(nd.adcConfigCode).label}` +
    (nd.decimationFactor > 1 ? ` decim=${nd.decimationFactor}` : '') +
    (nd.notchEnabled ? ` notch=N${nd.notchHarm}` : '') +
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
  panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
}

function adcCountsToVolts(counts, nd) {
  return counts / (nd?.countsPerVolt || cfg.ADC_COUNTS_PER_VOLT);
}

function handleData(nd, pkt) {
  const rawVal = adcCountsToVolts(pkt.value24, nd);

  if (!nd.gotFirst) {
    nd.gotFirst = true;
    nd.tFirst = performance.now() / 1000;
  }

  const evicted = nd.rawBuf.push(rawVal);
  nd.rawSum += rawVal - (evicted ?? 0);
  nd.totalSamples++;
  if (nd.totalSamples % cfg.SAMPLES_PER_BATCH === 0) nd.batchCount++;

  // The filtered signal is a full-capture product: keep it blank/dirty while
  // raw is still arriving so FIR/filtFilt and harmonic LS see the whole signal.
  nd.captureActive = true;
  nd.filterDirty = true;
  nd.filterReady = false;
  if (nd.filtBuf.length) nd.filtBuf.clear();
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
  // Cancelación de ruido de línea: LS de armónicos de 50 Hz sobre la ventana
  // completa, y(n)−f(n). Último paso de la cadena (FIR → DC → notch), igual
  // que el scope Python; queda también en snapshots/export, flageado en
  // metadata como notch_enabled/notch_harm.
  if (nd.notchEnabled && arr.length > 1) {
    const fs = nd.fs > 0 ? nd.fs : currentFsHz();
    arr = harmonicNotch(arr, fs, cfg.LINE_NOTCH_F0, nd.notchHarm, cfg.LINE_NOTCH_SEARCH_HZ);
  }
  return arr;
}

function storedFilteredArray(nd) {
  return nd && nd.filtBuf.length ? nd.filtBuf.toArray() : new Float64Array(0);
}

function filteredArrayForSnapshot(chIndex, rawArray = null) {
  const nd = data.nodes[chIndex];
  if (!nd) return new Float64Array(0);
  if (nd.filterDirty && !nd.captureActive) {
    reprocessFiltBuf(chIndex, { force: true, log: false });
  }
  return storedFilteredArray(nd);
}

function syncZeroPhaseFiltBuffers() {
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (!data.nodes[i].captureActive) reprocessFiltBuf(i, { force: true, log: false });
  }
}

function reprocessFiltBuf(chIndex, options = {}) {
  const nd = data.nodes[chIndex];
  const raw = nd.rawBuf.toArray();
  if (!raw.length) {
    nd.filtBuf.clear();
    nd.filtZi = null;
    nd.filterDirty = false;
    nd.filterReady = false;
    return;
  }

  if (nd.captureActive && !options.force) {
    nd.filtBuf.clear();
    nd.filtZi = null;
    nd.filterDirty = true;
    nd.filterReady = false;
    return;
  }

  const arr = filteredArrayForNode(nd, raw);
  nd.filtZi = null;
  fillRingBuffer(nd.filtBuf, arr);
  nd.filterDirty = false;
  nd.filterReady = true;
}

function finalizeNodeFilter(chIndex, reason = '') {
  const nd = data.nodes[chIndex];
  if (!nd || !nd.rawBuf.length) return false;
  nd.captureActive = false;
  reprocessFiltBuf(chIndex, { force: true });
  const panel = panelFor(chIndex);
  if (panel && nd.filtB && nd.filtCmd && nd.filterReady) panel.setFirStatus(`${nd.filtB.length} taps ${nd.filtCmd}`);
  return nd.filterReady;
}

function finalizeCompletedRaw(reason = '') {
  let count = 0;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const nd = data.nodes[i];
    if (!nd.rawBuf.length || (!nd.filterDirty && nd.filterReady)) continue;
    if (finalizeNodeFilter(i, reason)) count++;
  }
  if (count) {
    appendLog(`Filtrado final (${reason || 'captura completa'}): ${count} nodo(s)`);
    preserveCurrentCapture(reason || 'captura completa');
  }
}

function markNodeFilterDirty(chIndex) {
  const nd = data.nodes[chIndex];
  if (!nd) return;
  nd.filterDirty = nd.rawBuf.length > 0;
  nd.filterReady = false;
  nd.filtBuf.clear();
  if (!nd.captureActive) reprocessFiltBuf(chIndex, { force: true });
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
  // Reflejar y persistir la N derivada de la Fs efectiva en el control de
  // captura (Fs = 2604/N). Reemplaza el viejo campo "Fs manual".
  syncDecimFieldFromFs(fsHz);
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
  const rawN = loadSetting(GLOBAL_DECIMATION_KEY);
  const parsedN = rawN === null ? NaN : parseInt(rawN, 10);
  if (Number.isFinite(parsedN) && parsedN >= 1 && parsedN <= 100) {
    const n = saveGlobalDecimation(parsedN);
    const inp = $('capture-decim-n');
    if (inp) inp.value = String(n);
    const fsHz = Math.floor(cfg.DEFAULT_SAMPLE_RATE_HZ / n);
    updateDecimPreview();
    applyGlobalFs(fsHz, `N=${n} restaurado de cache local (Fs ${fsHz} Hz)`);
    return;
  }
  const raw = loadSetting(GLOBAL_FS_KEY);
  const fsHz = raw === null ? NaN : parseFloat(raw);
  if (!(fsHz > 0)) return;
  applyGlobalFs(fsHz, `Fs restaurado de cache local: ${fsHz} Hz`);
  syncDecimFieldFromFs(fsHz);
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

function schedulePgaoutLockTimeout(chIndex, expectedCode) {
  if (pgaoutLockTimers.has(chIndex)) clearTimeout(pgaoutLockTimers.get(chIndex));
  const timer = setTimeout(() => {
    pgaoutLockTimers.delete(chIndex);
    const nd = data.nodes[chIndex];
    const pending = nd.pending.get(cfg.SUBCMD_PGAOUT);
    if (!pending || pending.param !== expectedCode) return;
    nd.pending.delete(cfg.SUBCMD_PGAOUT);
    const panel = panelFor(chIndex);
    if (panel) panel.setPgaoutLock(2);
    appendLog(`S${chIndex} PGAout sin confirmacion: ${cfg.GAIN_NAMES[expectedCode] ?? expectedCode}`);
  }, Math.round(cfg.RETRY_SEC * 1000));
  pgaoutLockTimers.set(chIndex, timer);
}

function scheduleAdcConfigLockTimeout(chIndex, expectedCode) {
  if (adcConfigLockTimers.has(chIndex)) clearTimeout(adcConfigLockTimers.get(chIndex));
  const timer = setTimeout(() => {
    adcConfigLockTimers.delete(chIndex);
    const nd = data.nodes[chIndex];
    const pending = nd.pending.get(cfg.SUBCMD_ADC_CONFIG);
    if (!pending || pending.param !== expectedCode) return;
    nd.pending.delete(cfg.SUBCMD_ADC_CONFIG);
    const panel = panelFor(chIndex);
    if (panel) panel.setAdcConfigLock(2);
    appendLog(`S${chIndex} rango ADC sin confirmacion: ${adcConfigForCode(expectedCode).label}`);
  }, Math.round(cfg.RETRY_SEC * 1000));
  adcConfigLockTimers.set(chIndex, timer);
}

function scheduleDecimationLockTimeout(chIndex, expectedFactor) {
  if (decimationLockTimers.has(chIndex)) clearTimeout(decimationLockTimers.get(chIndex));
  const timer = setTimeout(() => {
    decimationLockTimers.delete(chIndex);
    const nd = data.nodes[chIndex];
    const pending = nd.pending.get(cfg.SUBCMD_DECIMATION);
    if (!pending || pending.param !== expectedFactor) return;
    nd.pending.delete(cfg.SUBCMD_DECIMATION);
    const panel = panelFor(chIndex);
    if (panel) panel.setDecimationLock(2);
    appendLog(`S${chIndex} decimacion sin confirmacion: ${expectedFactor}`);
  }, Math.round(cfg.RETRY_SEC * 1000));
  decimationLockTimers.set(chIndex, timer);
}

function scheduleSdEnableLockTimeout(chIndex, expectedEnabled) {
  if (sdEnableLockTimers.has(chIndex)) clearTimeout(sdEnableLockTimers.get(chIndex));
  const timer = setTimeout(() => {
    sdEnableLockTimers.delete(chIndex);
    const nd = data.nodes[chIndex];
    const pending = nd.pending.get(cfg.SUBCMD_SD_CAPTURE);
    if (!pending || pending.param !== (expectedEnabled ? 1 : 0)) return;
    nd.pending.delete(cfg.SUBCMD_SD_CAPTURE);
    const panel = panelFor(chIndex);
    if (panel) panel.setSdLock(2);
    appendLog(`S${chIndex} SD sin confirmacion: ${expectedEnabled ? 'ON' : 'OFF'}`);
  }, Math.round(cfg.RETRY_SEC * 1000));
  sdEnableLockTimers.set(chIndex, timer);
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

function onPgaoutChanged(chIndex, pgaoutCode) {
  const nd = data.nodes[chIndex];
  nd.pgaoutCode = clamp(pgaoutCode, 0, cfg.GAIN_CODES.length - 1);
  nd.pending.set(cfg.SUBCMD_PGAOUT, { param: nd.pgaoutCode, sendTime: performance.now(), retries: 0 });
  saveSlaveSetting(chIndex, 'pgaout_code', nd.pgaoutCode);
  const panel = panelFor(chIndex);
  if (panel) panel.setPgaoutLock(2);
  sendDirected(chIndex, cfg.SUBCMD_PGAOUT, nd.pgaoutCode);
  schedulePgaoutLockTimeout(chIndex, nd.pgaoutCode);
  appendLog(`S${chIndex} PGAout -> ${cfg.GAIN_NAMES[nd.pgaoutCode]}`);
}

function onAdcConfigChanged(chIndex, adcCode) {
  const nd = data.nodes[chIndex];
  const adcCfg = applyNodeAdcConfig(nd, adcCode);
  nd.pending.set(cfg.SUBCMD_ADC_CONFIG, { param: adcCfg.code, sendTime: performance.now(), retries: 0 });
  saveSlaveSetting(chIndex, 'adc_config_code', adcCfg.code);
  const panel = panelFor(chIndex);
  if (panel) {
    panel.setAdcConfig(adcCfg.code);
    panel.setAdcConfigLock(2);
  }
  sendDirected(chIndex, cfg.SUBCMD_ADC_CONFIG, adcCfg.code);
  scheduleAdcConfigLockTimeout(chIndex, adcCfg.code);
  renderNodeRow(chIndex);
  appendLog(`S${chIndex} rango ADC -> ${adcCfg.label}`);
}

function onDecimationChanged(chIndex, factor) {
  const nd = data.nodes[chIndex];
  const panel = panelFor(chIndex);
  const f = clamp(parseInt(factor, 10) || 1, 1, 100);
  // El esclavo descarta configs en silencio durante captura/volcado: rechazar
  // ya en la UI y volver al valor confirmado, en vez de fingir que aplico.
  if (masterBusyForStart()) {
    if (panel) {
      panel.setDecimation(nd.decimationFactor);
      panel.setDecimationLock(2);
    }
    appendLog(`S${chIndex} decimacion rechazada: master ${masterStateName()} (reintentar en reposo)`);
    return;
  }
  // nd.decimationFactor NO se toca aca: se confirma solo con el ACK ok=1
  // (handleAck), que es cuando el PSoC realmente cambio de Fs.
  const sendIt = () => {
    nd.pending.set(cfg.SUBCMD_DECIMATION, { param: f, sendTime: performance.now(), retries: 0 });
    if (panel) panel.setDecimationLock(2);
    sendDirected(chIndex, cfg.SUBCMD_DECIMATION, f);
    scheduleDecimationLockTimeout(chIndex, f);
    appendLog(`S${chIndex} decimacion -> ${f} (${Math.floor(cfg.DEFAULT_SAMPLE_RATE_HZ / f)} Hz)`);
  };
  // Serializar detras de cualquier config pendiente: el esclavo atiende un
  // config del PSoC a la vez (g_cfg_waiting) y rechazaba este 0xBB con ok=0.
  const waitStartMs = performance.now();
  const waitSettle = () => {
    const busy = nd.pending.has(cfg.SUBCMD_PGA) ||
                 nd.pending.has(cfg.SUBCMD_ADC_CONFIG) ||
                 nd.pending.has(cfg.SUBCMD_DECIMATION) ||
                 nd.pending.has(cfg.SUBCMD_SD_CAPTURE);
    if (busy && performance.now() - waitStartMs < cfg.RETRY_SEC * 2000) {
      setTimeout(waitSettle, 50);
      return;
    }
    sendIt();
  };
  waitSettle();
}

function onSdEnabledChanged(chIndex, enabled) {
  const nd = data.nodes[chIndex];
  if (!nd.sdPresent) {
    appendLog(`S${chIndex} SD ignorado: nodo sin SD detectada`);
    return;
  }
  const e = applyNodeSdEnabled(nd, enabled);
  nd.pending.set(cfg.SUBCMD_SD_CAPTURE, { param: e ? 1 : 0, sendTime: performance.now(), retries: 0 });
  const panel = panelFor(chIndex);
  if (panel) {
    panel.setSdEnabled(e);
    panel.setSdLock(2);
  }
  sendDirected(chIndex, cfg.SUBCMD_SD_CAPTURE, e ? 1 : 0);
  scheduleSdEnableLockTimeout(chIndex, e);
  appendLog(`S${chIndex} SD -> ${e ? 'ON' : 'OFF'}`);
}

function onVerRequested(chIndex) {
  if (!currentFsHz()) {
    appendLog(`VER Slave ${chIndex} cancelado: esperando Fs real del esclavo (HELLO no recibido aun)`);
    return;
  }
  const n = captureBatches();
  const secs = secondsForBatches(n);
  warnIfRamClamp(n, chIndex);
  resizePresentCaptureBuffers(n, chIndex);
  prepareNodeCapture(chIndex);
  sendHammerRecLen();
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
  const panel = panelFor(chIndex);

  sendDirected(chIndex, cfg.SUBCMD_PGA, nd.pgaCode);
  nd.pending.set(cfg.SUBCMD_PGA, { param: nd.pgaCode, sendTime: performance.now(), retries: 0 });
  if (panel) panel.setPgaLock(2);
  schedulePgaLockTimeout(chIndex, nd.pgaCode);

  /* PGAout viaja junto con el PGA en el "resend" — solo tiene sentido en GEO;
   * en un HAMMER el esclavo lo rechaza y el dot queda en "sin lock". */
  if (nd.hwClass === 0) {
    sendDirected(chIndex, cfg.SUBCMD_PGAOUT, nd.pgaoutCode);
    nd.pending.set(cfg.SUBCMD_PGAOUT, { param: nd.pgaoutCode, sendTime: performance.now(), retries: 0 });
    if (panel) panel.setPgaoutLock(2);
    schedulePgaoutLockTimeout(chIndex, nd.pgaoutCode);
  }

  // El esclavo solo atiende un config del PSoC a la vez (g_cfg_waiting en
  // main.cpp): si el rango ADC/decimacion se manda mientras el anterior
  // todavia espera su ACK, el esclavo lo rechaza en el acto (cfg busy, ok=0)
  // y nunca llega a aplicarse. Encadenar: PGA -> rango ADC -> decimacion,
  // esperando a que cada pending se resuelva (ack o timeout) antes del
  // siguiente envio.
  const waitStartMs = performance.now();
  const sendDecimationWhenAdcSettles = () => {
    const adcSettled = !nd.pending.has(cfg.SUBCMD_ADC_CONFIG);
    const waitedTooLong = performance.now() - waitStartMs > cfg.RETRY_SEC * 2000;
    if (!adcSettled && !waitedTooLong) {
      setTimeout(sendDecimationWhenAdcSettles, 50);
      return;
    }
    sendDirected(chIndex, cfg.SUBCMD_DECIMATION, nd.decimationFactor);
    nd.pending.set(cfg.SUBCMD_DECIMATION, { param: nd.decimationFactor, sendTime: performance.now(), retries: 0 });
    if (panel) panel.setDecimationLock(2);
    scheduleDecimationLockTimeout(chIndex, nd.decimationFactor);
  };
  const sendAdcConfigWhenPgaSettles = () => {
    const pgaSettled = !nd.pending.has(cfg.SUBCMD_PGA);
    const waitedTooLong = performance.now() - waitStartMs > cfg.RETRY_SEC * 1000;
    if (!pgaSettled && !waitedTooLong) {
      setTimeout(sendAdcConfigWhenPgaSettles, 50);
      return;
    }
    sendDirected(chIndex, cfg.SUBCMD_ADC_CONFIG, nd.adcConfigCode);
    nd.pending.set(cfg.SUBCMD_ADC_CONFIG, { param: nd.adcConfigCode, sendTime: performance.now(), retries: 0 });
    if (panel) panel.setAdcConfigLock(2);
    scheduleAdcConfigLockTimeout(chIndex, nd.adcConfigCode);
    sendDecimationWhenAdcSettles();
  };
  sendAdcConfigWhenPgaSettles();

  appendLog(`S${chIndex} enviar config: PGA ${cfg.GAIN_NAMES[nd.pgaCode] ?? nd.pgaCode}, ADC ${adcConfigForCode(nd.adcConfigCode).label}, decim ${nd.decimationFactor}`);
}

function onCalibrateRequested(chIndex) {
  const nd = data.nodes[chIndex];
  nd.calVdacs = emptyCalVdacs();
  nd.calVdacDetails = emptyCalVdacDetails();
  sendDirected(chIndex, cfg.SUBCMD_CALIBRATE, 1);
  nd.pending.set(cfg.SUBCMD_CALIBRATE, { param: 1, sendTime: performance.now(), retries: 0 });
  nd.calProgressLogMs = 0;
  const panel = panelFor(chIndex);
  if (panel) {
    panel.setCalibrationLock(3, 'solicitada');
    panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
  }
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

// Arma el ZIP de la captura. Lo comparten "Guardar CSV/ZIP" y "Subir al
// server": es exactamente el mismo paquete, solo cambia el destino.
function buildExportZip() {
  const displaySecs = parseInt($('disp-secs').value, 10) || null;
  const nBatches = captureBatches();
  const baseName = ($('export-name').value || cfg.DEFAULT_SAVE_NAME).trim() || cfg.DEFAULT_SAVE_NAME;
  syncZeroPhaseFiltBuffers();
  const liveCapture = makeCaptureSnapshot('Actual', { source: 'live', visible: false });
  const built = buildCaptureZip(data, {
    baseName,
    nSlaves: visibleSlaveCount(),
    nBatches,
    displaySecs,
    preservedCaptures: preservedCaptures.slice(),
    liveCapture: captureHasSamples(liveCapture) ? liveCapture : null,
  });
  return { ...built, liveCapture };
}

function markExported(liveCapture) {
  for (const capture of preservedCaptures) exportedCaptureSignatures.add(capture.signature);
  if (captureHasSamples(liveCapture)) exportedCaptureSignatures.add(liveCapture.signature);
}

function onExportRequested() {
  try {
    const { blob, filename, metadata, liveCapture } = buildExportZip();
    downloadBlob(blob, filename);
    markExported(liveCapture);
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

function onLineNotchChanged(chIndex, enabled, nHarm) {
  const nd = data.nodes[chIndex];
  nd.notchEnabled = !!enabled;
  nd.notchHarm = clampLineNotchHarm(nHarm);
  markNodeFilterDirty(chIndex);
  saveSlaveSetting(chIndex, 'notch_enabled', nd.notchEnabled ? 1 : 0);
  saveSlaveSetting(chIndex, 'notch_harm', nd.notchHarm);
  appendLog(`S${chIndex} ruido linea ${nd.notchEnabled ? 'ON' : 'OFF'} N=${nd.notchHarm}`);
}

function loadSlavePanelState(chIndex, panel) {
  const nd = data.nodes[chIndex];
  clearSlaveSetting(chIndex, 'alias');

  nd.pgaCode = loadSlaveInt(chIndex, 'pga_code', nd.pgaCode, 0, cfg.GAIN_CODES.length - 1);
  nd.pgaoutCode = loadSlaveInt(chIndex, 'pgaout_code', nd.pgaoutCode, 0, cfg.GAIN_CODES.length - 1);
  applyNodeAdcConfig(nd, loadSlaveInt(chIndex, 'adc_config_code', nd.adcConfigCode, 1, cfg.ADC_CONFIGS.length));
  applyNodeDecimation(nd, loadSlaveInt(chIndex, 'decimation_factor', nd.decimationFactor, 1, 100));
  nd.dcRemove = loadSlaveBool(chIndex, 'dc_remove', nd.dcRemove);

  panel.setPga(nd.pgaCode);
  panel.setPgaout(nd.pgaoutCode);
  panel.setPgaoutVisible(nd.hwClass === 0);
  panel.setAdcConfig(nd.adcConfigCode);
  panel.setDecimation(nd.decimationFactor);
  nd.hammerOffset = loadSlaveFloat(chIndex, 'hammer_offset_m', 0);
  if (nd.alias === 'Hammer') {
    nd.hammerOffset = 0;
    saveSlaveSetting(chIndex, 'hammer_offset_m', 0);
  }
  panel.setOffset(nd.hammerOffset);

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
  const legacyNotchEnabled = loadBoolSetting(`${SETTINGS_PREFIX}line_notch_on`, false);
  const legacyNotchHarm = clampLineNotchHarm(loadSetting(`${SETTINGS_PREFIX}line_notch_n`) ?? cfg.LINE_NOTCH_DEFAULT_HARM);
  nd.notchEnabled = loadSlaveBool(chIndex, 'notch_enabled', legacyNotchEnabled);
  nd.notchHarm = loadSlaveInt(chIndex, 'notch_harm', legacyNotchHarm, 1, cfg.LINE_NOTCH_MAX_HARM);
  panel.setLineNotch(nd.notchEnabled, nd.notchHarm);
  panel.updateStats(0, 0, null, nd.formatDriftStats(), nd.formatLatencyStats(), nd.psocOk);
}

function wireSlavePanel(chIndex, panel) {
  panel.addEventListener('pga-changed', (ev) => onPgaChanged(chIndex, ev.detail));
  panel.addEventListener('pgaout-changed', (ev) => onPgaoutChanged(chIndex, ev.detail));
  panel.addEventListener('adc-config-changed', (ev) => onAdcConfigChanged(chIndex, ev.detail.code));
  panel.addEventListener('decimation-changed', (ev) => onDecimationChanged(chIndex, ev.detail.factor));
  panel.addEventListener('sd-enable-changed', (ev) => onSdEnabledChanged(chIndex, !!ev.detail.enabled));
  panel.addEventListener('fir-apply', (ev) => onFirApply(chIndex, ev.detail.cmd));
  panel.addEventListener('fir-remove', () => onFirRemove(chIndex));
  panel.addEventListener('dc-remove-toggled', (ev) => onDcRemoveToggled(chIndex, !!ev.detail.enabled));
  panel.addEventListener('line-notch-changed', (ev) => onLineNotchChanged(
    chIndex,
    !!ev.detail.enabled,
    ev.detail.nHarm,
  ));
  panel.addEventListener('test-requested', () => onTestRequested(chIndex));
  panel.addEventListener('ver-requested', () => onVerRequested(chIndex));
  panel.addEventListener('send-all-requested', () => onSendAll(chIndex));
  panel.addEventListener('calibrate-requested', () => onCalibrateRequested(chIndex));
  panel.addEventListener('latency-requested', () => onLatencyRequested(chIndex));
  panel.addEventListener('blink-led-requested', () => onBlinkLedRequested(chIndex));
  panel.addEventListener('save-eeprom-requested', () => onSaveEepromRequested(chIndex));
  panel.addEventListener('fir-hw-toggled', (ev) => onFirHwToggled(chIndex, ev.detail.mode));
  panel.addEventListener('offset-changed', (ev) => {
    const nd = data.nodes[chIndex];
    if (nodeRole(nd) === 'hammer') {
      nd.hammerOffset = 0;
      panel.setOffset(0);
      saveSlaveSetting(chIndex, 'hammer_offset_m', 0);
      reorderGeosByOffset();
      return;
    }
    nd.hammerOffset = ev.detail;
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

function isGeoForOrdering(i) {
  const nd = data.nodes[i];
  if (!nd || !nodeHasPresence(i)) return false;
  return nodeRole(nd) === 'geo';
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
    if (isGeoForOrdering(i)) {
      geos.push({ idx: i, dist: nd.hammerOffset || 0 });
    }
  }
  // Sort by distance ascending (closest to source = Geo1)
  geos.sort((a, b) => (a.dist - b.dist) || (a.idx - b.idx));
  geos.forEach(({ idx }, rank) => {
    const name = `Geo${rank + 1}`;
    data.nodes[idx].alias = name;
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

  if (ackCmd === cfg.CMD_START) {
    if (ackVal) {
      appendLog('START aceptado por master');
    } else {
      appendLog(`START rechazado por master: estado=${masterStateName()} READY n=${activeSlaveCount}`);
      clearPendingStart();
    }
    return;
  }

  if (ackCmd === cfg.SUBCMD_SYNC_START && idx >= 1 && idx < cfg.MAX_NODES) {
    if (ackVal === 2) appendLog(`S${idx} START captura completa`);
    else if (ackVal === 0) appendLog(`S${idx} START fallo`);
    return;
  }

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

  if (ackCmd === cfg.SUBCMD_PGAOUT && idx >= 1 && idx < cfg.MAX_NODES) {
    const panel = panelFor(idx);
    if (panel) panel.setPgaoutLock(ackVal ? 1 : 2);
    if (pgaoutLockTimers.has(idx)) {
      clearTimeout(pgaoutLockTimers.get(idx));
      pgaoutLockTimers.delete(idx);
    }
    const expected = pending ? pending.param : data.nodes[idx].pgaoutCode;
    appendLog(`S${idx} PGAout ${ackVal ? 'confirmado' : 'sin lock'}: ${cfg.GAIN_NAMES[expected] ?? expected}`);
    return;
  }

  if (ackCmd === cfg.SUBCMD_ADC_CONFIG && idx >= 1 && idx < cfg.MAX_NODES) {
    const nd = data.nodes[idx];
    const panel = panelFor(idx);
    const expected = pending ? pending.param : nd.adcConfigCode;
    if (adcConfigLockTimers.has(idx)) {
      clearTimeout(adcConfigLockTimers.get(idx));
      adcConfigLockTimers.delete(idx);
    }
    if (ackVal) {
      const adcCfg = applyNodeAdcConfig(nd, expected);
      saveSlaveSetting(idx, 'adc_config_code', adcCfg.code);
      if (panel) {
        panel.setAdcConfig(adcCfg.code);
        panel.setAdcConfigLock(1);
      }
      renderNodeRow(idx);
      appendLog(`S${idx} rango ADC confirmado: ${adcCfg.label}`);
    } else {
      if (panel) panel.setAdcConfigLock(2);
      appendLog(`S${idx} rango ADC sin lock: ${adcConfigForCode(expected).label}`);
    }
    return;
  }

  if (ackCmd === cfg.SUBCMD_DECIMATION && idx >= 1 && idx < cfg.MAX_NODES) {
    const nd = data.nodes[idx];
    const panel = panelFor(idx);
    const expected = pending ? pending.param : nd.decimationFactor;
    if (decimationLockTimers.has(idx)) {
      clearTimeout(decimationLockTimers.get(idx));
      decimationLockTimers.delete(idx);
    }
    if (ackVal) {
      const f = applyNodeDecimation(nd, expected);
      saveSlaveSetting(idx, 'decimation_factor', f);
      if (panel) {
        panel.setDecimation(f);
        panel.setDecimationLock(1);
      }
      renderNodeRow(idx);
      appendLog(`S${idx} decimacion confirmada: ${f} (${Math.floor(cfg.DEFAULT_SAMPLE_RATE_HZ / f)} Hz)`);
    } else if (pending && (pending.retries ?? 0) < 1) {
      // ok=0 tipico: el esclavo estaba ocupado con otro config o el PSoC no
      // estaba IDLE todavia. Un unico reintento diferido suele alcanzar.
      const retry = { param: pending.param, sendTime: performance.now(), retries: 1 };
      nd.pending.set(cfg.SUBCMD_DECIMATION, retry);
      if (panel) panel.setDecimationLock(2);
      scheduleDecimationLockTimeout(idx, retry.param);
      setTimeout(() => sendDirected(idx, cfg.SUBCMD_DECIMATION, retry.param), 400);
      appendLog(`S${idx} decimacion ocupada, reintentando: ${retry.param}`);
    } else {
      // Rechazo definitivo: volver al valor CONFIRMADO para que la UI no
      // muestre una Fs que el PSoC nunca aplico.
      if (panel) {
        panel.setDecimation(nd.decimationFactor);
        panel.setDecimationLock(2);
      }
      appendLog(`S${idx} decimacion rechazada, se mantiene ${nd.decimationFactor}`);
    }
    return;
  }

  if (ackCmd === cfg.SUBCMD_SD_CAPTURE && idx >= 1 && idx < cfg.MAX_NODES) {
    const nd = data.nodes[idx];
    const panel = panelFor(idx);
    const expected = pending ? !!pending.param : nd.sdEnabled;
    if (sdEnableLockTimers.has(idx)) {
      clearTimeout(sdEnableLockTimers.get(idx));
      sdEnableLockTimers.delete(idx);
    }
    if (ackVal) {
      const e = applyNodeSdEnabled(nd, expected);
      if (panel) {
        panel.setSdEnabled(e);
        panel.setSdLock(1);
      }
      appendLog(`S${idx} SD confirmada: ${e ? 'ON' : 'OFF'}`);
    } else {
      if (panel) panel.setSdLock(2);
      appendLog(`S${idx} SD sin lock: ${expected ? 'ON' : 'OFF'}`);
    }
    return;
  }

  if (ackCmd >= cfg.SUBCMD_CAL_VDAC_BASE &&
      ackCmd < cfg.SUBCMD_CAL_VDAC_BASE + cfg.CAL_VDAC_STAGE_COUNT &&
      idx >= 1 && idx < cfg.MAX_NODES) {
    const stage = ackCmd - cfg.SUBCMD_CAL_VDAC_BASE;
    const nd = data.nodes[idx];
    setCalVdacCode(nd, stage, ackVal);
    const panel = panelFor(idx);
    if (panel) panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
    appendLog(`S${idx} VDAC ${stage + 1}: ${ackVal} (${vdacCodeMv(ackVal)} mV)`);
    return;
  }

  if (ackCmd >= cfg.SUBCMD_CAL_TARGET_MV_HI_BASE &&
      ackCmd < cfg.SUBCMD_CAL_TARGET_MV_HI_BASE + cfg.CAL_VDAC_STAGE_COUNT &&
      idx >= 1 && idx < cfg.MAX_NODES) {
    const stage = ackCmd - cfg.SUBCMD_CAL_TARGET_MV_HI_BASE;
    const nd = data.nodes[idx];
    setCalVdacSignedWord(nd, stage, 'target_mV', 'hi', ackVal);
    const panel = panelFor(idx);
    if (panel) panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
    return;
  }

  if (ackCmd >= cfg.SUBCMD_CAL_TARGET_MV_LO_BASE &&
      ackCmd < cfg.SUBCMD_CAL_TARGET_MV_LO_BASE + cfg.CAL_VDAC_STAGE_COUNT &&
      idx >= 1 && idx < cfg.MAX_NODES) {
    const stage = ackCmd - cfg.SUBCMD_CAL_TARGET_MV_LO_BASE;
    const nd = data.nodes[idx];
    setCalVdacSignedWord(nd, stage, 'target_mV', 'lo', ackVal);
    const panel = panelFor(idx);
    if (panel) panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
    return;
  }

  if (ackCmd >= cfg.SUBCMD_CAL_ERROR_MV_HI_BASE &&
      ackCmd < cfg.SUBCMD_CAL_ERROR_MV_HI_BASE + cfg.CAL_VDAC_STAGE_COUNT &&
      idx >= 1 && idx < cfg.MAX_NODES) {
    const stage = ackCmd - cfg.SUBCMD_CAL_ERROR_MV_HI_BASE;
    const nd = data.nodes[idx];
    setCalVdacSignedWord(nd, stage, 'error_mV', 'hi', ackVal);
    const panel = panelFor(idx);
    if (panel) panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
    return;
  }

  if (ackCmd >= cfg.SUBCMD_CAL_ERROR_MV_LO_BASE &&
      ackCmd < cfg.SUBCMD_CAL_ERROR_MV_LO_BASE + cfg.CAL_VDAC_STAGE_COUNT &&
      idx >= 1 && idx < cfg.MAX_NODES) {
    const stage = ackCmd - cfg.SUBCMD_CAL_ERROR_MV_LO_BASE;
    const nd = data.nodes[idx];
    setCalVdacSignedWord(nd, stage, 'error_mV', 'lo', ackVal);
    const panel = panelFor(idx);
    if (panel) panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
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

  if (pkt.helloMacSub === 0x07) {
    const nd = data.nodes[idx];
    const present = !!pkt.helloSdPresent;
    if (nd.sdPresent !== present) {
      nd.sdPresent = present;
      nd.sdEnabled = present; /* firmware GEO auto-habilita SD al montar FAT */
      const panel = panelFor(idx);
      if (panel) panel.setSdPresent(present, nd.sdEnabled);
      appendLog(`HELLO slave=${idx} sd_present=${present ? 1 : 0}`);
    }
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
  const filtBufs = data.nodes.map((nd) => (nd.filtBuf.length ? nd.filtBuf.toArray() : null));
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
  if (!$('spectra').hidden) {
    const spectrumSources = rawBufsDisplay.map((raw, i) => buildSpectrumSources(raw, filtBufsDisplay[i]));
    const spectrumOverlays = overlays.map((capture) => ({
      ...capture,
      nodes: (capture.nodes || []).map((node) => (
        node ? { ...node, spectrumSources: buildSpectrumSources(node.raw, node.filt) } : node
      )),
    }));
    spectrumArea.update(spectrumSources, fs, spectrumOverlays);
  }

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
initEnlaceTab(appendLog);
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
  if (!ev.detail) clearPendingStart();
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

// Intensidad de señal — el maestro solo manda esto fuera de captura (ver
// link_rssi.h); no hace falta filtrar por estado acá, el último valor
// mostrado queda "congelado" en la UI mientras dura la adquisición.
ws.addEventListener('link-rssi', (ev) => {
  const msg = ev.detail;
  const wifiEl = $('wifi-rssi');
  if (wifiEl) {
    wifiEl.textContent = Number.isFinite(msg.wifi_rssi)
      ? `${msg.wifi_rssi} dBm (${msg.wifi_clients ?? 0} cliente/s)`
      : '--';
  }
  for (const slave of msg.slaves || []) {
    const panel = panelFor(slave.node);
    if (panel) panel.setRssi(Number.isFinite(slave.rssi) ? slave.rssi : null);
  }
});

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
      const prevState = masterState;
      masterState = pkt.hbMasterState;
      setMasterState(masterState);
      if (prevState === cfg.MASTER_STATE_DUMPING && masterState !== cfg.MASTER_STATE_DUMPING) {
        finalizeCompletedRaw('dump completo');
      } else if (prevState === cfg.MASTER_STATE_RUNNING &&
                 ![cfg.MASTER_STATE_RUNNING, cfg.MASTER_STATE_STOPPING, cfg.MASTER_STATE_DUMPING].includes(masterState)) {
        finalizeCompletedRaw('stream detenido');
      }
    } else {
      const panel = panelFor(idx);
      if (panel) {
        panel.setPga(nd.pgaCode);
        panel.setVdacs(nd.calVdacs, nd.hwClass, nd.vdacByte, nd.calVdacDetails);
      }
    }
  } else if (pkt.isAck) {
    handleAck(pkt, idx);
  } else if (pkt.isReady) {
    setActiveSlaveCount(pkt.readyNSlaves);
    appendLog(`READY n_slaves=${pkt.readyNSlaves}`);
    maybeRunPendingStart(pkt.readyNSlaves);
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
  clearPendingStart();
  setActiveSlaveCount(0);
  sendStd(cfg.CMD_ARM, n);
  appendLog(`ARM solicitado n=${n || 'todos'}; esperando READY real`);
});

// N (decimación) global — reemplaza el viejo "Fs manual". El ADC del PSoC corre
// siempre a 2604 Hz nativos; la Fs efectiva se deriva del factor de decimación
// N (Fs = 2604/N, promediando N muestras). El operador fija N y se aplica a
// todos los nodos conectados (mismo camino que el control por-esclavo:
// SUBCMD_DECIMATION con ACK/lock), además de alimentar el badge de Fs global.
function captureDecimN() {
  const inp = $('capture-decim-n');
  return clamp(parseInt(inp ? inp.value : '1', 10) || 1, 1, 100);
}

function updateDecimPreview() {
  const n = captureDecimN();
  const span = $('decim-fs-preview');
  if (span) span.textContent = `${Math.floor(cfg.DEFAULT_SAMPLE_RATE_HZ / n)} Hz`;
  return n;
}

// Mantener el campo N y su preview en sync cuando la Fs efectiva cambia por
// HELLO/cache (Fs = 2604/N ⇒ N = round(2604/Fs)). Si el operador ya aplicó
// una N global, conservar esa preferencia en el control hasta que la cambie:
// un HELLO de arranque no debe destruir el valor persistido en localStorage.
function syncDecimFieldFromFs(fsHz) {
  if (!(fsHz > 0)) return;
  const inp = $('capture-decim-n');
  if (!inp) return;
  const rawSaved = loadSetting(GLOBAL_DECIMATION_KEY);
  const saved = rawSaved === null ? NaN : parseInt(rawSaved, 10);
  const n = Number.isFinite(saved) && saved >= 1 && saved <= 100
    ? saved
    : clamp(Math.round(cfg.DEFAULT_SAMPLE_RATE_HZ / fsHz), 1, 100);
  inp.value = String(n);
  const span = $('decim-fs-preview');
  if (span) span.textContent = `${Math.floor(cfg.DEFAULT_SAMPLE_RATE_HZ / n)} Hz`;
}

function applyGlobalDecimation() {
  const n = updateDecimPreview();
  const inp = $('capture-decim-n');
  if (inp) inp.value = String(n);
  const fsHz = Math.floor(cfg.DEFAULT_SAMPLE_RATE_HZ / n);
  saveGlobalDecimation(n);
  saveGlobalFs(fsHz);
  applyGlobalFs(fsHz, `N=${n} → Fs ${fsHz} Hz`);
  let sent = 0;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const nd = data.nodes[i];
    if (nodeConnectedForCapture(nd, i)) {
      onDecimationChanged(i, n);
      sent++;
    }
  }
  appendLog(`N=${n} aplicado a ${sent} nodo(s), Fs ${fsHz} Hz`);
}

$('capture-decim-n').addEventListener('input', updateDecimPreview);
$('btn-apply-decim').addEventListener('click', applyGlobalDecimation);

$('btn-start').addEventListener('click', () => {
  if (!currentFsHz()) {
    appendLog('START cancelado: esperando Fs real del esclavo (HELLO no recibido aun)');
    return;
  }
  if (masterBusyForStart()) {
    appendLog(`START bloqueado: master ${masterStateName()}, envia STOP antes de otro START`);
    return;
  }
  const req = makeStartRequestInfo();
  if (activeSlaveCount > 0 || masterState === cfg.MASTER_STATE_ARMED) {
    runStartCapture(req);
    return;
  }
  const visibleCount = visibleSlaveCount();
  if (visibleCount <= 0) {
    appendLog('START cancelado: no hay esclavos visibles para armar');
    return;
  }
  queueStartAfterArm(req, visibleCount);
});

$('btn-preserve').addEventListener('click', onDiscountRequested);

$('btn-stop').addEventListener('click', () => {
  clearPendingStart();
  sendStd(cfg.CMD_STOP, 0);
  sendStd(cfg.CMD_STREAM, 0);
});

$('btn-cal-all').addEventListener('click', () => {
  if (!ws.connected) return;
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].psocOk !== null) onCalibrateRequested(i);
  }
  appendLog('Calibracion solicitada a todos los esclavos');
});

$('btn-export').addEventListener('click', onExportRequested);

// "Subir al server": el mismo ZIP, pero POSTeado al servidor de datos en vez de
// bajarlo. Ojo con quien hace este pedido: lo hace el NAVEGADOR, no el maestro
// — el ESP nunca sale a internet. La direccion, eso si, la guarda el maestro
// (pestana Enlace) para no tener que cargarla en cada cliente que abra la SPA.
async function onUploadToServer() {
  const url = getServerUrl();
  if (!url) {
    $('export-status').textContent =
      'Falta la URL del servidor: cargala en la pestaña Enlace y guardá';
    return;
  }
  let built;
  try {
    built = buildExportZip();
  } catch (err) {
    $('export-status').textContent = `Error armando el ZIP: ${err && err.message ? err.message : err}`;
    return;
  }
  const { blob, filename, metadata, liveCapture } = built;
  const kb = (blob.size / 1024).toFixed(0);
  $('export-status').textContent = `Subiendo ${filename} (${kb} kB)…`;
  appendLog(`Subiendo ${filename} (${kb} kB) a ${url}`);
  try {
    const r = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/zip', 'X-Geo-Filename': filename },
      body: blob,
    });
    const txt = (await r.text()).trim();
    if (!r.ok) {
      $('export-status').textContent = `El servidor rechazó la subida (HTTP ${r.status}): ${txt.slice(0, 120)}`;
      appendLog(`Subida rechazada: HTTP ${r.status}`);
      return;
    }
    // Recien con el 2xx del servidor se marca como exportada: si la subida
    // fallo, la captura sigue pendiente y no se pierde de vista.
    markExported(liveCapture);
    const captureCount = metadata.capture_count ?? 1;
    $('export-status').textContent = `Subido: ${filename} (${captureCount} capturas) — ${txt.slice(0, 80)}`;
    appendLog(`Subida OK: ${filename}`);
  } catch (err) {
    // Caso tipico: el navegador esta en el AP del maestro, que no tiene salida,
    // o el maestro esta en fase de captura y el telefono perdio la ruta.
    $('export-status').textContent =
      `No se pudo alcanzar el servidor (${err && err.message ? err.message : err}). ` +
      `¿Estás en la red del cliente, con la VPN levantada?`;
    appendLog('Subida fallida: servidor inalcanzable');
  }
}

$('btn-upload-server').addEventListener('click', onUploadToServer);
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
  if (active) {
    spectrumArea.resetView();
  }
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

const HAMMER_SECS_KEY = `${SETTINGS_PREFIX}hammer_secs_v1`;

function onHammerDurationEdited() {
  const secs = hammerCaptureSeconds();
  saveSetting(HAMMER_SECS_KEY, secs === null ? '' : String(secs));
  updateCapturePreview();
  /* Sincronizar el largo HAMMER al maestro apenas se edita, así un VER o un
   * START posterior ya lo encuentran configurado. */
  sendHammerRecLen();
}

function restoreHammerDuration() {
  const raw = loadSetting(HAMMER_SECS_KEY);
  const el = $('capture-secs-hammer');
  if (!el) return;
  const secs = raw === null || raw === '' ? NaN : parseFloat(raw);
  el.value = Number.isFinite(secs) && secs > 0 ? String(secs) : '';
}

$('disp-secs').addEventListener('input', applyDisplayWindow);
$('disp-secs').addEventListener('change', applyDisplayWindow);
$('capture-secs').addEventListener('input', onCaptureDurationEdited);
$('capture-secs').addEventListener('change', onCaptureDurationEdited);
$('capture-secs-hammer').addEventListener('input', updateCapturePreview);
$('capture-secs-hammer').addEventListener('change', onHammerDurationEdited);
$('btn-max-batches').addEventListener('click', setCaptureToMaxBatches);
$('btn-max-ram').addEventListener('click', setCaptureToMaxRamBatches);
restoreHammerDuration();
syncDisplayWindowToCaptureDuration();

// Avisar antes de cerrar/recargar si hay mediciones capturadas que todavia
// no se bajaron en un ZIP — perderlas silenciosamente ya causo problemas
// en campo. El navegador ignora el texto de returnValue y muestra su propio
// dialogo generico; igual lo seteamos por compatibilidad con motores viejos.
window.addEventListener('beforeunload', (e) => {
  if (!hasUnexportedData()) return;
  e.preventDefault();
  e.returnValue = 'Hay mediciones capturadas sin exportar. Si salis ahora se pierden.';
});

ws.start();
