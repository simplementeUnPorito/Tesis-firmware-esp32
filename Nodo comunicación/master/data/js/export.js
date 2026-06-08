// export.js - browser-side capture export.

import * as cfg from './config.js';
import { buildStoreZip } from './zip_store.js';
import { effectiveFs } from './data_store.js';

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
  for (let i = 0; i < values.length; i++) lines.push(`${i},${(i / fs).toFixed(9)},${values[i]}`);
  return lines.join('\n') + '\n';
}

function gainValue(code) {
  return (code >= 0 && code < cfg.GAIN_CODES.length) ? cfg.GAIN_CODES[code] : null;
}

function nodeMetadata(nd, index, rawPath, filtPath, csvPaths) {
  return {
    index,
    name: cfg.NODE_NAMES[index] || `Node ${index}`,
    slave_id: nd.slaveId,
    fs: nd.fs,
    fs_known: !!nd.fsKnown,
    raw_file: rawPath,
    filt_file: filtPath,
    raw_csv_file: csvPaths ? csvPaths.raw : null,
    filt_csv_file: csvPaths ? csvPaths.filt : null,
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
    mac: nd.mac || '',
    visible: !!nd.visible,
    fir_cmd: nd.filtCmd || '',
    dc_remove: !!nd.dcRemove,
    notch_enabled: !!nd.notchEnabled,
    notch_harm: nd.notchHarm,
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
  const includeCsv = !!options.includeCsv;
  const files = [];
  const nodes = [];
  const visibleNodes = [];

  for (let i = 0; i < dataStore.nodes.length; i++) {
    const nd = dataStore.nodes[i];
    const raw = nd.rawBuf.toArray();
    const filt = nd.filtBuf.toArray();
    const dir = `node${i}`;
    const rawPath = `${dir}/raw_f32le.bin`;
    const filtPath = `${dir}/filt_f32le.bin`;
    files.push({ name: rawPath, data: float32Bytes(raw) });
    files.push({ name: filtPath, data: float32Bytes(filt) });

    let csvPaths = null;
    if (includeCsv) {
      csvPaths = { raw: `${dir}/raw.csv`, filt: `${dir}/filt.csv` };
      files.push({ name: csvPaths.raw, data: csvText(raw, nd.fs || cfg.FS) });
      files.push({ name: csvPaths.filt, data: csvText(filt, nd.fs || cfg.FS) });
    }

    if (i > 0 && i <= nSlaves && nd.visible) visibleNodes.push(i);
    nodes.push(nodeMetadata(nd, i, rawPath, filtPath, csvPaths));
  }

  const metadata = {
    schema: 'geophone_scope_web_zip_v1',
    schema_target: 'geophone_scope_mat_node_prefix_v1',
    created_at: now.toISOString(),
    save_time: stamp,
    source: 'esp32_master_web_ui',
    web_app_version: 'phase5',
    fs: effectiveFs(dataStore),
    n_slaves: nSlaves,
    n_batches: options.nBatches ?? 0,
    samples_per_batch: cfg.SAMPLES_PER_BATCH,
    signal_units: 'V',
    adc_counts_per_volt: cfg.ADC_COUNTS_PER_VOLT,
    vdac_step_volts: cfg.VDAC_STEP,
    notch_f0: cfg.NOTCH_F0,
    display_secs: options.displaySecs ?? null,
    max_buf_secs: options.maxBufSecs ?? cfg.MAX_BUF_S,
    visible_nodes: visibleNodes,
    dtype: 'float32',
    endian: 'little',
    nodes,
  };
  files.unshift({ name: 'metadata.json', data: JSON.stringify(metadata, null, 2) + '\n' });

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
