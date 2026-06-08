// app.js - bootstraps the web UI. Mirrors the orchestration role of
// gui/main_window.py: WebSocket packets -> DataStore/UI, and UI actions ->
// the same command bytes that handleMatlabCmd() already consumes.

import * as cfg from './config.js';
import { WsClient } from './ws_client.js';
import { encodeStd, encodeStd16, encodeDirected } from './protocol.js';
import { DataStore, effectiveFs } from './data_store.js';
import { PlotArea } from './plot.js?v=plot-zoom-1';
import { SlavePanel } from './slave_panel.js';
import { compileFirCmd, firFilter, harmonicNotch, lastFirError } from './signal_proc.js';
import { buildCaptureZip, downloadBlob } from './export.js';

const $ = (id) => document.getElementById(id);

const ws = new WsClient(`ws://${location.host}/ws`);
const data = new DataStore();
const plotArea = new PlotArea($('plots'));
const slavePanels = new Array(cfg.MAX_NODES).fill(null);
const macPartial = new Map();
const pgaLockTimers = new Map();
const testTimers = new Map();

let masterState = 0;
let activeSlaveCount = 0;

const SETTINGS_PREFIX = 'geophone_scope_web.';

function clamp(value, lo, hi) {
  return Math.max(lo, Math.min(hi, value));
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

function anySlaveFsKnown() {
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].fsKnown) return true;
  }
  return false;
}

function currentFsHz() {
  return effectiveFs(data);
}

function captureSeconds() {
  const el = $('capture-secs');
  const parsed = el ? parseFloat(el.value) : NaN;
  const secs = Number.isFinite(parsed) ? parsed : 1.0;
  return clamp(secs, 0.1, 120);
}

function batchesForSeconds(seconds) {
  const fs = currentFsHz();
  const batches = Math.ceil((seconds * fs) / cfg.SAMPLES_PER_BATCH);
  return clamp(Math.max(1, batches), 1, cfg.PSOC_CAPTURE_MAX_BATCHES);
}

function captureBatches() {
  return batchesForSeconds(captureSeconds());
}

function secondsForBatches(nBatches) {
  return (nBatches * cfg.SAMPLES_PER_BATCH) / currentFsHz();
}

function updateCapturePreview() {
  const el = $('capture-batches');
  if (!el) return;
  const secs = captureSeconds();
  const n = batchesForSeconds(secs);
  const actualSecs = secondsForBatches(n);
  const capped = secs > actualSecs + 0.01 ? ' max' : '';
  el.textContent = `${n} lotes / real ${actualSecs.toFixed(2)} s${capped}`;
}

function updateGlobalFsDisplay() {
  const el = $('global-fs');
  if (el) {
    const suffix = anySlaveFsKnown() ? '' : ' nominal';
    el.textContent = `${currentFsHz().toFixed(0)} Hz${suffix}`;
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

function setActiveSlaveCount(nSlaves, zeroMeansAll = false) {
  const maxSlaves = cfg.MAX_NODES - 1;
  const parsed = Number.isFinite(nSlaves) ? nSlaves : 0;
  const count = (zeroMeansAll && parsed === 0)
    ? maxSlaves
    : clamp(parsed, 0, maxSlaves);
  activeSlaveCount = count;
  const visibleNodes = [];
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    const visible = i <= count;
    data.nodes[i].visible = visible;
    if (visible) visibleNodes.push(i);
    const row = $(`node-${i}`);
    if (row) row.hidden = !visible;
    const panel = panelFor(i);
    if (panel) panel.setVisible(visible);
  }
  plotArea.setActiveNodes(visibleNodes);
  updateCapturePreview();
}

function applyDisplayWindow() {
  const secs = parseInt($('disp-secs').value, 10) || 1;
  plotArea.setDisplaySamples(Math.round(secs * currentFsHz()));
  updateCapturePreview();
}

function syncDataBufferForFs() {
  const maxSamples = Math.max(
    cfg.SAMPLES_PER_BATCH,
    Math.round(cfg.MAX_BUF_S * currentFsHz()),
  );
  data.resizeAll(maxSamples);
}

function renderNodeRow(i) {
  if (i <= 0 || i >= cfg.MAX_NODES) return;
  const nd = data.nodes[i];
  const row = $(`node-${i}`);
  if (!row) return;
  const tail = nd.rawBuf.tail(1);
  const raw = tail.length ? tail[0] : null;
  row.querySelector('.raw').textContent = (raw === null) ? '--' : `${raw.toFixed(4)} V`;
  row.querySelector('.stats').textContent = `${nd.batchCount} lotes / ${nd.totalSamples} muestras`;
  row.querySelector('.cfg').textContent =
    `pga=${cfg.GAIN_NAMES[nd.pgaCode] ?? nd.pgaCode} vdac=${nd.vdacByte}` +
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

  // FIR + DC removal stream sample-by-sample. The harmonic notch fits the
  // complete buffer, so renderTick() rebuilds filtBuf while it is enabled.
  if (!nd.notchEnabled) {
    let filtVal = rawVal;
    if (nd.filtB) {
      const result = firFilter(nd.filtB, new Float64Array([rawVal]), nd.filtZi);
      nd.filtZi = result.zi;
      filtVal = result.y[0];
    }
    if (nd.dcRemove && nd.rawBuf.length > 1) filtVal -= nd.rawSum / nd.rawBuf.length;
    nd.filtBuf.push(filtVal);
  }
}

function fillRingBuffer(ring, arr) {
  ring.clear();
  for (let i = 0; i < arr.length; i++) ring.push(arr[i]);
}

function reprocessFiltBuf(chIndex) {
  const nd = data.nodes[chIndex];
  const raw = nd.rawBuf.toArray();
  if (!raw.length) {
    nd.filtBuf.clear();
    nd.filtZi = null;
    return;
  }

  let arr;
  if (nd.filtB) {
    const result = firFilter(nd.filtB, raw, null);
    arr = result.y;
    nd.filtZi = result.zi;
  } else {
    arr = new Float64Array(raw);
    nd.filtZi = null;
  }

  if (nd.dcRemove && raw.length > 1) {
    let mean = 0;
    for (let i = 0; i < raw.length; i++) mean += raw[i];
    mean /= raw.length;
    const out = new Float64Array(arr.length);
    for (let i = 0; i < arr.length; i++) out[i] = arr[i] - mean;
    arr = out;
  }

  if (nd.notchEnabled) arr = harmonicNotch(arr, nd.fs, cfg.NOTCH_F0, nd.notchHarm);
  fillRingBuffer(nd.filtBuf, arr);
}

function recompileNodeFirForFs(chIndex) {
  const nd = data.nodes[chIndex];
  if (!nd.filtCmd) return true;
  const b = compileFirCmd(nd.filtCmd, nd.fs);
  const panel = panelFor(chIndex);
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

function updateNodeFsFromHello(chIndex, fsHz) {
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
  if (!changed && wasKnown) return;

  syncDataBufferForFs();
  applyDisplayWindow();
  updateGlobalFsDisplay();
  for (let i = 1; i < cfg.MAX_NODES; i++) renderNodeRow(i);
  appendLog(`Fs global actualizado por S${chIndex}: ${fsHz} Hz`);
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
  if (chIndex > activeSlaveCount) setActiveSlaveCount(chIndex);
  const nd = data.nodes[chIndex];
  nd.clear();
  nd.visible = true;
  plotArea.clearNode(chIndex);
  renderNodeRow(chIndex);
  updateSlavePanelStats(chIndex);
  return nd;
}

function onVdacChanged(chIndex, vdacByte) {
  const nd = data.nodes[chIndex];
  nd.vdacByte = clamp(vdacByte, cfg.VDAC_MIN, cfg.VDAC_MAX);
  nd.pending.set(cfg.SUBCMD_VDAC, { param: nd.vdacByte, sendTime: performance.now(), retries: 0 });
  saveSlaveSetting(chIndex, 'vdac_byte', nd.vdacByte);
  const panel = panelFor(chIndex);
  if (panel) panel.setVdacLock(2);
  sendDirected(chIndex, cfg.SUBCMD_VDAC, nd.vdacByte);
  appendLog(`S${chIndex} VDAC -> ${nd.vdacByte}`);
}

function onPgavdacChanged(chIndex, pgaCode) {
  const nd = data.nodes[chIndex];
  nd.pgavdac = clamp(pgaCode, 0, cfg.GAIN_CODES.length - 1);
  saveSlaveSetting(chIndex, 'pgavdac_code', nd.pgavdac);
  sendDirected(chIndex, cfg.SUBCMD_PGAVDAC, nd.pgavdac);
  appendLog(`S${chIndex} PGAvdac -> ${cfg.GAIN_NAMES[nd.pgavdac]}`);
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
  sendDirected(chIndex, cfg.SUBCMD_PGAVDAC, nd.pgavdac);
  sendDirected(chIndex, cfg.SUBCMD_VDAC, nd.vdacByte);
  sendDirected(chIndex, cfg.SUBCMD_PGA, nd.pgaCode);
  nd.pending.set(cfg.SUBCMD_VDAC, { param: nd.vdacByte, sendTime: performance.now(), retries: 0 });
  nd.pending.set(cfg.SUBCMD_PGA, { param: nd.pgaCode, sendTime: performance.now(), retries: 0 });
  const panel = panelFor(chIndex);
  if (panel) {
    panel.setVdacLock(2);
    panel.setPgaLock(2);
  }
  schedulePgaLockTimeout(chIndex, nd.pgaCode);
  appendLog(`S${chIndex} enviar todo: pgavdac=${nd.pgavdac} vdac=${nd.vdacByte} pga=${nd.pgaCode}`);
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
    for (let i = 0; i < cfg.MAX_NODES; i++) {
      if (data.nodes[i].notchEnabled) reprocessFiltBuf(i);
    }
    const displaySecs = parseInt($('disp-secs').value, 10) || null;
    const nBatches = captureBatches();
    const baseName = ($('export-name').value || cfg.DEFAULT_SAVE_NAME).trim() || cfg.DEFAULT_SAVE_NAME;
    const { blob, filename, metadata } = buildCaptureZip(data, {
      baseName,
      nSlaves: activeSlaveCount,
      nBatches,
      displaySecs,
      includeCsv: $('chk-export-csv').checked,
    });
    downloadBlob(blob, filename);
    $('export-status').textContent =
      `ZIP: ${filename} (${metadata.nodes.reduce((s, n) => s + n.raw_count, 0)} muestras raw)`;
    appendLog(`Export ZIP ${filename}`);
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

function onNotchToggled(chIndex, enabled) {
  const nd = data.nodes[chIndex];
  nd.notchEnabled = !!enabled;
  reprocessFiltBuf(chIndex);
  saveSlaveSetting(chIndex, 'notch_enabled', nd.notchEnabled ? 1 : 0);
}

function onNotchHarmChanged(chIndex, harmonics) {
  const nd = data.nodes[chIndex];
  nd.notchHarm = clamp(parseInt(harmonics, 10) || cfg.NOTCH_DEFAULT_HARM, 1, 5);
  if (nd.notchEnabled) reprocessFiltBuf(chIndex);
  saveSlaveSetting(chIndex, 'notch_harm', nd.notchHarm);
}

function loadSlavePanelState(chIndex, panel) {
  const nd = data.nodes[chIndex];
  const alias = loadSetting(settingKey(chIndex, 'alias'));
  if (alias) panel.setAlias(alias);

  nd.pgavdac = loadSlaveInt(chIndex, 'pgavdac_code', nd.pgavdac, 0, cfg.GAIN_CODES.length - 1);
  nd.vdacByte = loadSlaveInt(chIndex, 'vdac_byte', nd.vdacByte, cfg.VDAC_MIN, cfg.VDAC_MAX);
  nd.pgaCode = loadSlaveInt(chIndex, 'pga_code', nd.pgaCode, 0, cfg.GAIN_CODES.length - 1);
  nd.dcRemove = loadSlaveBool(chIndex, 'dc_remove', nd.dcRemove);
  nd.notchEnabled = loadSlaveBool(chIndex, 'notch_enabled', nd.notchEnabled);
  nd.notchHarm = loadSlaveInt(chIndex, 'notch_harm', nd.notchHarm, 1, 5);

  panel.setPgavdac(nd.pgavdac);
  panel.setVdac(nd.vdacByte);
  panel.setPga(nd.pgaCode);
  panel.setDcRemove(nd.dcRemove);
  panel.setNotchEnabled(nd.notchEnabled);
  panel.setNotchHarmonics(nd.notchHarm);
  panel.setFirPreset(
    loadSetting(settingKey(chIndex, 'fir_type')) || 'lp',
    loadSlaveFloat(chIndex, 'fir_f1', 10),
    loadSlaveFloat(chIndex, 'fir_f2', 200),
    loadSlaveInt(chIndex, 'fir_taps', 101, 3, 401),
  );
  const firCmd = loadSetting(settingKey(chIndex, 'fir_cmd')) || '';
  if (firCmd.trim()) {
    const b = compileFirCmd(firCmd, nd.fs);
    if (b) {
      nd.filtB = b;
      nd.filtCmd = firCmd;
      panel.setFirStatus(`${b.length} taps ${firCmd}`);
    } else {
      panel.setFirStatus(`Invalid: ${lastFirError()}`.slice(0, 80));
    }
  }
  panel.updateStats(0, 0, null, nd.formatDriftStats(), nd.formatLatencyStats(), nd.psocOk);
}

function wireSlavePanel(chIndex, panel) {
  panel.addEventListener('alias-changed', (ev) => {
    saveSlaveSetting(chIndex, 'alias', ev.detail);
  });
  panel.addEventListener('vdac-changed', (ev) => onVdacChanged(chIndex, ev.detail));
  panel.addEventListener('pgavdac-changed', (ev) => onPgavdacChanged(chIndex, ev.detail));
  panel.addEventListener('pga-changed', (ev) => onPgaChanged(chIndex, ev.detail));
  panel.addEventListener('fir-apply', (ev) => onFirApply(chIndex, ev.detail.cmd));
  panel.addEventListener('fir-remove', () => onFirRemove(chIndex));
  panel.addEventListener('dc-remove-toggled', (ev) => onDcRemoveToggled(chIndex, !!ev.detail.enabled));
  panel.addEventListener('notch-toggled', (ev) => onNotchToggled(chIndex, !!ev.detail.enabled));
  panel.addEventListener('notch-harm-changed', (ev) => onNotchHarmChanged(chIndex, ev.detail.harmonics));
  panel.addEventListener('test-requested', () => onTestRequested(chIndex));
  panel.addEventListener('ver-requested', () => onVerRequested(chIndex));
  panel.addEventListener('send-all-requested', () => onSendAll(chIndex));
  panel.addEventListener('latency-requested', () => onLatencyRequested(chIndex));
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

function handleAck(pkt, idx) {
  const ackCmd = pkt.ackCmd;
  const ackVal = pkt.ackVal;
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
    const panel = panelFor(idx);
    if (panel) panel.setVdacLock(ackVal ? 1 : 2);
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

  if (pkt.helloMacSub === 0x01) {
    const nd = data.nodes[idx];
    nd.psocOk = pkt.helloPsocOk;
    updateNodeFsFromHello(idx, pkt.helloFsHz);
    updateSlavePanelStats(idx);
    renderNodeRow(idx);
    appendLog(`HELLO slave=${idx} psoc_ok=${pkt.helloPsocOk} fs=${pkt.helloFsHz || '?'}Hz`);
  }
}

function renderTick() {
  const fs = effectiveFs(data);
  for (let i = 1; i < cfg.MAX_NODES; i++) {
    if (data.nodes[i].notchEnabled) reprocessFiltBuf(i);
  }
  const rawBufs = data.nodes.map((nd) => (nd.rawBuf.length ? nd.rawBuf.toArray() : null));
  const filtBufs = data.nodes.map((nd) => (nd.filtBuf.length ? nd.filtBuf.toArray() : null));
  plotArea.update(rawBufs, filtBufs, fs);

  updateGlobalFsDisplay();
  for (let i = 1; i <= activeSlaveCount; i++) {
    renderNodeRow(i);
    updateSlavePanelStats(i);
  }
}

function applyTheme(light) {
  document.body.classList.toggle('light', light);
  $('btn-theme').textContent = light ? 'Modo oscuro' : 'Modo claro';
}

function initTheme() {
  const saved = loadSetting(`${SETTINGS_PREFIX}theme`);
  applyTheme(saved === 'light');
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
buildSlavePanels();
setConnIndicator(false);
setMasterState(masterState);
setActiveSlaveCount(activeSlaveCount);
updateGlobalFsDisplay();
setInterval(renderTick, cfg.RENDER_PERIOD_MS);

ws.addEventListener('connection', (ev) => {
  setConnIndicator(ev.detail);
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
        panel.setVdac(nd.vdacByte);
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
});

// Controls

$('btn-connect').addEventListener('click', () => ws.start());

$('btn-arm').addEventListener('click', () => {
  const n = clamp(parseInt($('arm-n').value, 10) || 0, 0, cfg.MAX_NODES - 1);
  setActiveSlaveCount(n, true);
  sendStd(cfg.CMD_ARM, n);
});

$('btn-start').addEventListener('click', () => {
  const n = captureBatches();
  const secs = secondsForBatches(n);
  data.clearAll();
  plotArea.clearAll();
  sendStd16(cfg.CMD_START, n);
  appendLog(`START: ${n} lotes (~${secs.toFixed(2)} s real)`);
});

$('btn-stop').addEventListener('click', () => {
  sendStd(cfg.CMD_STOP, 0);
  sendStd(cfg.CMD_STREAM, 0);
});

$('btn-status').addEventListener('click', () => {
  sendStd(cfg.CMD_STATUS, 0);
});

$('btn-export').addEventListener('click', onExportRequested);

$('btn-plot-zoom-in').addEventListener('click', () => plotArea.zoomBy(1.5, 0.5));
$('btn-plot-zoom-out').addEventListener('click', () => plotArea.zoomBy(1 / 1.5, 0.5));
$('btn-plot-reset').addEventListener('click', () => plotArea.resetView());

$('disp-secs').addEventListener('change', applyDisplayWindow);
$('capture-secs').addEventListener('change', () => {
  updateCapturePreview();
  applyDisplayWindow();
});
applyDisplayWindow();

ws.start();
