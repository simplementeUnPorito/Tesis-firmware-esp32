// export.js - browser-side capture export.

import * as cfg from './config.js?v=field-loop-22';
import { buildStoreZip } from './zip_store.js';
import { effectiveFs } from './data_store.js?v=field-loop-22';

function compactTimestamp(date) {
  const pad = (n) => String(n).padStart(2, '0');
  return `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}_` +
         `${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
}

function float32Bytes(values) {
  const src = ArrayBuffer.isView(values) ? values : new Float64Array(values);
  const out = new ArrayBuffer(src.length * 4);
  const view = new DataView(out);
  for (let i = 0; i < src.length; i++) view.setFloat32(i * 4, src[i], true);
  return new Uint8Array(out);
}

function csvText(values, fs) {
  const lines = ['sample,time_s,value_v'];
  const fsKnown = fs > 0;
  for (let i = 0; i < values.length; i++) lines.push(`${i},${fsKnown ? (i / fs).toFixed(9) : ''},${values[i]}`);
  return lines.join('\n') + '\n';
}

function gainValue(code) {
  return (code >= 0 && code < cfg.GAIN_CODES.length) ? cfg.GAIN_CODES[code] : null;
}

function slugPart(value, fallback) {
  const text = String(value || fallback || '').trim().toLowerCase();
  const slug = text
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[^a-z0-9]+/g, '_')
    .replace(/^_+|_+$/g, '');
  return slug || fallback;
}

function uniqueDir(base, used, index) {
  let dir = base;
  if (used.has(dir)) dir = `${base}_s${index}`;
  let n = 2;
  while (used.has(dir)) dir = `${base}_s${index}_${n++}`;
  used.add(dir);
  return dir;
}

function slaveConnectedForExport(nd, index, nSlaves) {
  return index > 0
    && index <= nSlaves
    && nd.visible
    && (nd.fsKnown || !!nd.mac || nd.rawBuf.length > 0 || nd.filtBuf.length > 0);
}

function slaveTypeRank(alias) {
  const idx = cfg.SLAVE_TYPE_ORDER.indexOf(String(alias || '').trim());
  return idx >= 0 ? idx : cfg.SLAVE_TYPE_ORDER.length;
}

function orderedSlaveIndices(dataStore, limit = cfg.MAX_NODES - 1) {
  const maxIndex = Math.min(dataStore.nodes.length - 1, limit);
  const indices = [];
  for (let i = 1; i <= maxIndex; i++) indices.push(i);
  indices.sort((a, b) => {
    const ar = slaveTypeRank(dataStore.nodes[a]?.alias);
    const br = slaveTypeRank(dataStore.nodes[b]?.alias);
    return (ar - br) || (a - b);
  });
  return indices;
}

function nodeMetadata(nd, index, paths, connected) {
  return {
    index,
    name: cfg.NODE_NAMES[index] || `Node ${index}`,
    type: nd.alias || cfg.NODE_NAMES[index] || `Node ${index}`,
    slave_id: nd.slaveId,
    fs: nd.fs,
    fs_known: !!nd.fsKnown,
    connected: !!connected,
    data_dir: paths ? paths.dir : null,
    raw_file: paths ? paths.raw : null,
    filt_file: paths ? paths.filt : null,
    raw_csv_file: paths ? paths.rawCsv : null,
    filt_csv_file: paths ? paths.filtCsv : null,
    raw_count: nd.rawBuf.length,
    filt_count: nd.filtBuf.length,
    batch_count: nd.batchCount,
    total_samples: nd.totalSamples,
    pga_code: nd.pgaCode,
    pga_gain: gainValue(nd.pgaCode),
    vdac_byte: nd.vdacByte,
    pgavdac_code: nd.pgavdac,
    pgavdac_gain: gainValue(nd.pgavdac),
    psoc_ok: nd.psocOk,
    hammer_offset_m: nd.hammerOffset ?? 0,
    mac: nd.mac || '',
    visible: !!nd.visible,
    fir_cmd: nd.filtCmd || '',
    dc_remove: !!nd.dcRemove,
    drift_hist: nd.driftHist.slice(),
    latency_hist: nd.latencyHist.slice(),
    health: nd.health,
    units: 'V',
    dtype: 'float32',
    endian: 'little',
  };
}

export function buildCaptureZip(dataStore, options = {}) {
  const now = new Date();
  const stamp = compactTimestamp(now);
  const nSlaves = options.nSlaves ?? (cfg.MAX_NODES - 1);
  const files = [];
  const nodes = [];
  const visibleNodes = [];
  const exportedNodes = [];
  const usedDirs = new Set(['maestro']);
  const connectedByNode = new Map();
  const pathsByNode = new Map();

  for (const i of orderedSlaveIndices(dataStore, nSlaves)) {
    const nd = dataStore.nodes[i];
    const connected = slaveConnectedForExport(nd, i, nSlaves);
    connectedByNode.set(i, connected);
    if (!connected) continue;

    const raw = nd.rawBuf.toArray();
    const filt = nd.filtBuf.toArray();
    const dirBase = slugPart(nd.alias || cfg.NODE_NAMES[i], `slave${i}`);
    const dir = uniqueDir(dirBase, usedDirs, i);
    const paths = {
      dir,
      raw: `${dir}/raw_f32le.bin`,
      filt: `${dir}/filt_f32le.bin`,
      rawCsv: `${dir}/raw.csv`,
      filtCsv: `${dir}/filt.csv`,
    };
    pathsByNode.set(i, paths);
    files.push({ name: paths.raw, data: float32Bytes(raw) });
    files.push({ name: paths.filt, data: float32Bytes(filt) });

    // CSV is always saved alongside the binary dump (no opt-out): it's the
    // human-readable record needed to reconstruct/study a capture later.
    // nd.fs is only ever the value reported by the slave's HELLO - never a
    // guessed/nominal constant - so the CSV time column matches reality.
    files.push({ name: paths.rawCsv, data: csvText(raw, nd.fs) });
    files.push({ name: paths.filtCsv, data: csvText(filt, nd.fs) });

    visibleNodes.push(i);
    exportedNodes.push(i);
  }

  nodes.push(nodeMetadata(dataStore.nodes[0], 0, null, false));
  for (const i of orderedSlaveIndices(dataStore, cfg.MAX_NODES - 1)) {
    const nd = dataStore.nodes[i];
    const connected = connectedByNode.has(i)
      ? connectedByNode.get(i)
      : slaveConnectedForExport(nd, i, nSlaves);
    nodes.push(nodeMetadata(nd, i, pathsByNode.get(i) || null, connected));
  }

  const metadata = {
    schema: 'geophone_scope_web_zip_v2',
    schema_target: 'geophone_scope_mat_node_prefix_v1',
    created_at: now.toISOString(),
    save_time: stamp,
    source: 'esp32_master_web_ui',
    web_app_version: 'phase5',
    layout: 'maestro_metadata_plus_slave_type_dirs',
    fs: effectiveFs(dataStore),
    n_slaves: nSlaves,
    n_batches: options.nBatches ?? 0,
    samples_per_batch: cfg.SAMPLES_PER_BATCH,
    signal_units: 'V',
    adc_counts_per_volt: cfg.ADC_COUNTS_PER_VOLT,
    vdac_step_volts: cfg.VDAC_STEP,
    display_secs: options.displaySecs ?? null,
    max_buf_secs: options.maxBufSecs ?? cfg.MAX_BUF_S,
    visible_nodes: visibleNodes,
    exported_nodes: exportedNodes,
    dtype: 'float32',
    endian: 'little',
    nodes,
  };

  const masterConfig = {
    created_at: metadata.created_at,
    save_time: metadata.save_time,
    fs: metadata.fs,
    n_slaves: metadata.n_slaves,
    n_batches: metadata.n_batches,
    samples_per_batch: metadata.samples_per_batch,
    display_secs: metadata.display_secs,
    max_buf_secs: metadata.max_buf_secs,
    active_slave_indices: visibleNodes,
    exported_slave_indices: exportedNodes,
    slaves: nodes.filter((node) => node.index > 0),
  };
  const metadataJson = JSON.stringify(metadata, null, 2) + '\n';
  files.unshift({ name: 'maestro/config.json', data: JSON.stringify(masterConfig, null, 2) + '\n' });
  files.unshift({ name: 'maestro/metadata.json', data: metadataJson });
  files.unshift({ name: 'metadata.json', data: metadataJson });

  return {
    blob: buildStoreZip(files),
    filename: `${options.baseName || cfg.DEFAULT_SAVE_NAME}_${stamp}.zip`,
    metadata,
  };
}

export function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1500);
}
