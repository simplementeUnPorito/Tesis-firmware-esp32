// export.js - browser-side capture export.

import * as cfg from './config.js?v=field-study-3';
import { buildStoreZip } from './zip_store.js';
import { effectiveFs } from './data_store.js?v=field-study-3';

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

function csvText(values, fs, sampleOffset = 0) {
  const offset = Number.isFinite(sampleOffset) ? Math.round(sampleOffset) : 0;
  const lines = ['sample,time_s,value_v,sample_offset,aligned_sample,aligned_time_s'];
  const fsKnown = fs > 0;
  for (let i = 0; i < values.length; i++) {
    const aligned = i + offset;
    lines.push([
      i,
      fsKnown ? (i / fs).toFixed(9) : '',
      values[i],
      offset,
      aligned,
      fsKnown ? (aligned / fs).toFixed(9) : '',
    ].join(','));
  }
  return lines.join('\n') + '\n';
}

function csvCell(value) {
  if (value === null || value === undefined) return '';
  const text = String(value);
  return /[",\n\r]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

function csvRow(values) {
  return values.map(csvCell).join(',');
}

function signalValues(node, signal) {
  const values = node ? node[signal] : null;
  if (!values) return new Float64Array(0);
  return ArrayBuffer.isView(values) ? values : new Float64Array(values);
}

function sampleOffset(value) {
  return Number.isFinite(value) ? Math.round(value) : 0;
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
    sample_offset: 0,
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

function captureNodeMetadata(node, index, paths, connected, captureOffset = 0) {
  const raw = signalValues(node, 'raw');
  const filt = signalValues(node, 'filt');
  return {
    index,
    name: node?.name || cfg.NODE_NAMES[index] || `Node ${index}`,
    type: node?.type || cfg.NODE_NAMES[index] || `Node ${index}`,
    slave_id: node?.slave_id || (index === 0 ? 'M' : `S${index}`),
    fs: node?.fs ?? 0,
    fs_known: !!node?.fs_known,
    connected: !!connected,
    data_dir: paths ? paths.dir : null,
    raw_file: paths ? paths.raw : null,
    filt_file: paths ? paths.filt : null,
    raw_csv_file: paths ? paths.rawCsv : null,
    filt_csv_file: paths ? paths.filtCsv : null,
    raw_count: raw.length,
    filt_count: filt.length,
    batch_count: node?.batch_count ?? 0,
    total_samples: node?.total_samples ?? raw.length,
    pga_code: node?.pga_code ?? 0,
    pga_gain: node?.pga_gain ?? gainValue(node?.pga_code ?? 0),
    vdac_byte: node?.vdac_byte ?? 128,
    pgavdac_code: node?.pgavdac_code ?? 0,
    pgavdac_gain: node?.pgavdac_gain ?? gainValue(node?.pgavdac_code ?? 0),
    psoc_ok: node?.psoc_ok ?? null,
    hammer_offset_m: node?.hammer_offset_m ?? 0,
    sample_offset: sampleOffset(captureOffset),
    mac: node?.mac || '',
    visible: !!node?.visible,
    fir_cmd: node?.fir_cmd || '',
    filt_trim_samples: node?.filt_trim_samples ?? 0,
    dc_remove: !!node?.dc_remove,
    drift_hist: Array.isArray(node?.drift_hist) ? node.drift_hist.slice() : [],
    latency_hist: Array.isArray(node?.latency_hist) ? node.latency_hist.slice() : [],
    health: node?.health ?? 0,
    units: 'V',
    dtype: 'float32',
    endian: 'little',
  };
}

function captureHasSamples(capture) {
  return !!capture && Array.isArray(capture.nodes) && capture.nodes.some((node) =>
    signalValues(node, 'raw').length > 0 || signalValues(node, 'filt').length > 0);
}

function buildCombinedSignalsCsv(captures) {
  const lines = [csvRow([
    'capture_order',
    'capture_id',
    'capture_label',
    'capture_source',
    'node_index',
    'node_type',
    'slave_id',
    'signal',
    'sample',
    'time_s',
    'value_v',
    'sample_offset',
    'aligned_sample',
    'aligned_time_s',
    'fs_hz',
    'hammer_offset_m',
    'pga_code',
    'pga_gain',
    'vdac_byte',
    'mac',
  ])];

  for (const capture of captures) {
    const offset = sampleOffset(capture.sample_offset);
    for (const node of capture.nodes || []) {
      if (!node || node.index <= 0) continue;
      const fs = node.fs || capture.fs || 0;
      for (const signal of ['raw', 'filt']) {
        const values = signalValues(node, signal);
        for (let i = 0; i < values.length; i++) {
          lines.push(csvRow([
            capture.order ?? '',
            capture.id || '',
            capture.label || '',
            capture.source || '',
            node.index,
            node.type || '',
            node.slave_id || '',
            signal,
            i,
            fs > 0 ? (i / fs).toFixed(9) : '',
            values[i],
            offset,
            i + offset,
            fs > 0 ? ((i + offset) / fs).toFixed(9) : '',
            fs || '',
            node.hammer_offset_m ?? 0,
            node.pga_code ?? '',
            node.pga_gain ?? '',
            node.vdac_byte ?? '',
            node.mac || '',
          ]));
        }
      }
    }
  }
  return lines.join('\n') + '\n';
}

function buildCaptureSummaryCsv(captures) {
  const lines = [csvRow([
    'capture_order',
    'capture_id',
    'capture_label',
    'capture_source',
    'created_at',
    'fs_hz',
    'n_slaves',
    'raw_count',
    'filt_count',
    'sample_offset',
    'visible',
  ])];
  for (const capture of captures) {
    lines.push(csvRow([
      capture.order ?? '',
      capture.id || '',
      capture.label || '',
      capture.source || '',
      capture.created_at || '',
      capture.fs || '',
      capture.n_slaves ?? '',
      capture.raw_count ?? 0,
      capture.filt_count ?? 0,
      sampleOffset(capture.sample_offset),
      capture.visible ? 1 : 0,
    ]));
  }
  return lines.join('\n') + '\n';
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
  const preservedCaptures = Array.isArray(options.preservedCaptures)
    ? options.preservedCaptures.filter(captureHasSamples)
    : [];
  const liveCapture = captureHasSamples(options.liveCapture) ? options.liveCapture : null;
  const exportCaptures = preservedCaptures.slice();
  if (liveCapture) {
    const duplicate = exportCaptures.some((capture) =>
      capture.signature && liveCapture.signature && capture.signature === liveCapture.signature);
    if (!duplicate || exportCaptures.length === 0) exportCaptures.push(liveCapture);
  }

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
    files.push({ name: paths.rawCsv, data: csvText(raw, nd.fs, 0) });
    files.push({ name: paths.filtCsv, data: csvText(filt, nd.fs, 0) });

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

  const captureMetas = [];
  exportCaptures.forEach((capture, captureIdx) => {
    const captureOrder = capture.order || (captureIdx + 1);
    const captureSlug = slugPart(capture.label || capture.id || `capture_${captureOrder}`, `capture_${captureOrder}`);
    const captureDir = `captures/${String(captureIdx + 1).padStart(3, '0')}_${captureSlug}`;
    const capUsedDirs = new Set();
    const capNodeMetas = [];
    const sortedNodes = (capture.nodes || []).slice().sort((a, b) => (a.index ?? 0) - (b.index ?? 0));
    const captureOffset = sampleOffset(capture.sample_offset);

    for (const node of sortedNodes) {
      const index = node.index ?? 0;
      const raw = signalValues(node, 'raw');
      const filt = signalValues(node, 'filt');
      const hasData = raw.length > 0 || filt.length > 0;
      const connected = index > 0 && (!!node.connected || hasData);
      let paths = null;

      if (index > 0 && hasData) {
        const dirBase = `${captureDir}/${slugPart(node.type || cfg.NODE_NAMES[index], `slave${index}`)}`;
        const dir = uniqueDir(dirBase, capUsedDirs, index);
        paths = {
          dir,
          raw: `${dir}/raw_f32le.bin`,
          filt: `${dir}/filt_f32le.bin`,
          rawCsv: `${dir}/raw.csv`,
          filtCsv: `${dir}/filt.csv`,
        };
        files.push({ name: paths.raw, data: float32Bytes(raw) });
        files.push({ name: paths.filt, data: float32Bytes(filt) });
        files.push({ name: paths.rawCsv, data: csvText(raw, node.fs || capture.fs || 0, captureOffset) });
        files.push({ name: paths.filtCsv, data: csvText(filt, node.fs || capture.fs || 0, captureOffset) });
      }

      capNodeMetas.push(captureNodeMetadata(node, index, paths, connected, captureOffset));
    }

    const capMeta = {
      id: capture.id || `capture_${captureIdx + 1}`,
      order: capture.order ?? captureIdx + 1,
      label: capture.label || `Capture ${captureIdx + 1}`,
      source: capture.source || 'preserved',
      created_at: capture.created_at || null,
      save_time: capture.save_time || null,
      fs: capture.fs || 0,
      n_slaves: capture.n_slaves ?? nSlaves,
      n_batches: capture.n_batches ?? 0,
      samples_per_batch: capture.samples_per_batch ?? cfg.SAMPLES_PER_BATCH,
      display_secs: capture.display_secs ?? options.displaySecs ?? null,
      max_buf_secs: capture.max_buf_secs ?? cfg.MAX_BUF_S,
      visible: !!capture.visible,
      sample_offset: captureOffset,
      signature: capture.signature || null,
      data_dir: captureDir,
      metadata_file: `${captureDir}/metadata.json`,
      raw_count: capNodeMetas.reduce((sum, node) => sum + node.raw_count, 0),
      filt_count: capNodeMetas.reduce((sum, node) => sum + node.filt_count, 0),
      nodes: capNodeMetas,
    };
    files.push({ name: capMeta.metadata_file, data: JSON.stringify(capMeta, null, 2) + '\n' });
    captureMetas.push(capMeta);
  });

  const combinedSignalsPath = captureMetas.length ? 'combined/signals.csv' : null;
  const captureSummaryPath = captureMetas.length ? 'combined/captures.csv' : null;
  if (captureMetas.length) {
    files.push({ name: combinedSignalsPath, data: buildCombinedSignalsCsv(exportCaptures) });
    files.push({ name: captureSummaryPath, data: buildCaptureSummaryCsv(exportCaptures) });
  }

  const metadata = {
    schema: 'geophone_scope_web_zip_v3',
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
    capture_count: captureMetas.length || (exportedNodes.length ? 1 : 0),
    preserved_count: preservedCaptures.length,
    combined_csv_file: combinedSignalsPath,
    capture_summary_csv_file: captureSummaryPath,
    captures: captureMetas,
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
    capture_count: metadata.capture_count,
    preserved_count: metadata.preserved_count,
    combined_csv_file: metadata.combined_csv_file,
    capture_summary_csv_file: metadata.capture_summary_csv_file,
    captures: captureMetas.map((capture) => ({
      id: capture.id,
      order: capture.order,
      label: capture.label,
      source: capture.source,
      data_dir: capture.data_dir,
      metadata_file: capture.metadata_file,
      raw_count: capture.raw_count,
      filt_count: capture.filt_count,
      sample_offset: capture.sample_offset,
    })),
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
