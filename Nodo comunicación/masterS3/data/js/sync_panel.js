// Sincronizacion durante la captura y distancia estimada por FTM.
//
// El modo (FTM activo / FTM pausado / radio apagada) viaja al maestro con el
// comando 0xB8 antes de cada START. Los esclavos S3 informan sus sesiones FTM
// ({"type":"ftm"} por el WebSocket); de la mediana del RTT sale una distancia
// cruda c·RTT/2 que NO es una cinta metrica (mide 3-4 veces la real, ver
// docs/diseno_s3/OTA_FTM_DISENO.md §3). Se calibra por nodo contra distancias
// declaradas por el operador: d_declarada = escala·d_ftm (+ offset con >= 3
// distancias distintas). La declarada nunca se pisa con la estimada.

export const SYNC_MODES = [
  [0, 'FTM activo'],
  [1, 'FTM pausado (solo asincrónico)'],
  [2, 'Radio apagada (solo asincrónico con límite)'],
];
const SYNC_MODE_KEYS = ['ftm_active', 'ftm_paused', 'radio_off'];
const STATUS_NAMES = ['sin sesiones', 'ok', 'placa sin FTM', 'maestro sin FTM',
  'sin respuesta', 'falló'];
const C_M_PER_PS = 299792458e-12;
const STORE_KEY = 'geophone_scope_web.ftm_calibration';
const MODE_KEY = 'geophone_scope_web.sync_mode';

const reports = {};         // node -> último reporte
let calib = loadCalib();     // node -> { points: [[d_ftm, d_decl]], declared }
let snapshot = null;         // estado congelado al START

function loadCalib() {
  try { return JSON.parse(localStorage.getItem(STORE_KEY) || '{}') || {}; } catch (_) { return {}; }
}
function saveCalib() {
  try { localStorage.setItem(STORE_KEY, JSON.stringify(calib)); } catch (_) {}
}

export function loadSyncMode() {
  try {
    const v = parseInt(localStorage.getItem(MODE_KEY), 10);
    return v >= 0 && v <= 2 ? v : 0;
  } catch (_) { return 0; }
}
export function saveSyncMode(v) {
  try { localStorage.setItem(MODE_KEY, String(v)); } catch (_) {}
}

export function syncModeKey(v) { return SYNC_MODE_KEYS[v] || 'ftm_active'; }

function rawDistanceM(r) {
  return r && r.window > 0 ? r.rtt_med_ps * C_M_PER_PS / 2 : null;
}

/* Recta por mínimos cuadrados; con menos de tres distancias distintas, solo escala. */
export function fitCalibration(points) {
  const pts = (points || []).filter((p) => Number.isFinite(p[0]) && Number.isFinite(p[1]) && p[0] > 0);
  if (!pts.length) return null;
  const distinct = new Set(pts.map((p) => p[1].toFixed(2))).size;
  if (distinct >= 3) {
    const n = pts.length;
    const mx = pts.reduce((a, p) => a + p[0], 0) / n;
    const my = pts.reduce((a, p) => a + p[1], 0) / n;
    let sxx = 0; let sxy = 0;
    for (const [x, y] of pts) { sxx += (x - mx) ** 2; sxy += (x - mx) * (y - my); }
    if (sxx > 0) {
      const scale = sxy / sxx;
      const offset = my - scale * mx;
      const res = pts.map(([x, y]) => y - (scale * x + offset));
      return { scale, offset_m: offset, n_points: n, n_distances: distinct, model: 'scale_offset',
        max_abs_residual_m: Math.max(...res.map(Math.abs)) };
    }
  }
  const sxy = pts.reduce((a, p) => a + p[0] * p[1], 0);
  const sxx = pts.reduce((a, p) => a + p[0] * p[0], 0);
  const scale = sxy / sxx;
  const res = pts.map(([x, y]) => y - scale * x);
  return { scale, offset_m: 0, n_points: pts.length, n_distances: distinct, model: 'scale_only',
    max_abs_residual_m: Math.max(...res.map(Math.abs)) };
}

function nodeSummary(node) {
  const r = reports[node] || null;
  const c = calib[node] || {};
  const fit = fitCalibration(c.points);
  const raw = rawDistanceM(r);
  return {
    ftm_status: r ? (STATUS_NAMES[r.status] || String(r.status)) : 'sin reporte',
    ftm_sessions: r?.sessions ?? 0,
    ftm_frames_total: r?.frames_total ?? 0,
    ftm_frames_valid: r?.frames_valid ?? 0,
    ftm_window: r?.window ?? 0,
    rtt_median_ps: r && r.window > 0 ? r.rtt_med_ps : null,
    rtt_mad_ps: r && r.window > 0 ? r.rtt_mad_ps : null,
    rtt_p95_ps: r && r.window > 0 ? r.rtt_p95_ps : null,
    rssi_median_dbm: r && r.window > 0 ? r.rssi_med : null,
    channel: r?.channel ?? null,
    clock_skew_ppb: r && r.window >= 8 ? r.skew_ppb : null,
    clock_residual_std_ns: r && r.window >= 8 ? r.resid_ns : null,
    ftm_age_ms: r && r.age_ms !== 0xFFFFFFFF ? r.age_ms : null,
    distance_declared_m: Number.isFinite(c.declared) ? c.declared : null,
    distance_ftm_raw_m: raw,
    distance_ftm_sdk_m: r && r.dist_sdk_cm ? r.dist_sdk_cm / 100 : null,
    distance_ftm_calibrated_m: raw != null && fit ? fit.scale * raw + fit.offset_m : null,
    distance_pair: `S${node}-maestro`,
    calibration: fit ? { ...fit, points: c.points } : null,
    sample_timestamp_method: 'scheduled_index',
  };
}

/* Se llama al mandar START: congela modo y estado FTM de cada nodo. */
export function syncSnapshotForStart(mode) {
  const nodes = {};
  for (const n of Object.keys(reports)) nodes[n] = nodeSummary(Number(n));
  snapshot = { sync_mode: syncModeKey(mode), sync_mode_code: mode,
    sync_schema: 'geophone_sync_v1', started_at: new Date().toISOString(), nodes };
}

/* El maestro informa como salio el START: programado por el modelo FTM de
 * cada esclavo (todos con modelo valido) o broadcast como antes. */
export function onSyncStart(msg) {
  if (!snapshot) syncSnapshotForStart(loadSyncMode());
  snapshot.start = {
    method: msg.method === 'ftm_scheduled' ? 'ftm_scheduled' : 'broadcast_start',
    start_master_mac_us: Number.isFinite(msg.start_mac_us) ? msg.start_mac_us : null,
    lead_us: Number.isFinite(msg.lead_us) ? msg.lead_us : null,
    received_at: new Date().toISOString(),
  };
}

export function syncCaptureMetadata() {
  if (!snapshot) return null;
  const { nodes, start, ...rest } = snapshot;
  if (start?.method === 'ftm_scheduled') {
    return { ...rest, start_method: 'ftm_scheduled',
      start_master_mac_us: start.start_master_mac_us, start_lead_us: start.lead_us,
      note: 'START programado: cada esclavo sube SYNC en el instante indicado del reloj MAC del maestro, convertido con su modelo FTM.' };
  }
  return { ...rest, start_method: 'broadcast_start',
    note: start ? 'START por broadcast ESP-NOW: algun esclavo no tenia modelo FTM valido.'
      : 'START por broadcast ESP-NOW (el maestro no informo el metodo).' };
}

export function syncNodeMetadata(node) {
  return snapshot?.nodes?.[node] || null;
}

export function mountSyncPanel(container, log = () => {}) {
  if (!container || document.getElementById('sync-panel')) return null;
  const box = document.createElement('fieldset');
  box.id = 'sync-panel';
  box.className = 'panel';
  box.innerHTML = `
    <legend>Sincronización FTM y distancia al maestro</legend>
    <div class="sync-nodes"></div>
    <p class="muted">Solo los esclavos ESP32-S3 miden FTM, y el maestro también tiene que ser S3.
    La distancia FTM es una estimación sesgada: para calibrarla, medí con cinta la distancia
    del esclavo al maestro, escribila en «Declarada» y tocá «Punto». Hacelo en tres distancias
    o más para ajustar escala y offset.</p>`;
  container.append(box);
  const list = box.querySelector('.sync-nodes');
  const rows = {};

  const render = (node) => {
    let tr = rows[node];
    if (!tr) {
      tr = document.createElement('div');
      tr.className = 'sync-node';
      tr.innerHTML = `<div class="row"><b>S${node}</b> <span class="sync-ftm"></span></div>
        <div class="row"><span class="sync-dist"></span></div>
        <div class="row"><label>Declarada (m) <input type="number" step="0.01" min="0" class="small-number"></label>
        <button type="button">Punto</button> <button type="button">Borrar</button></div>`;
      const input = tr.querySelector('input');
      const [btnPoint, btnClear] = tr.querySelectorAll('button');
      input.value = Number.isFinite(calib[node]?.declared) ? String(calib[node].declared) : '';
      input.addEventListener('change', () => {
        const v = parseFloat(input.value);
        calib[node] = { ...(calib[node] || {}), declared: Number.isFinite(v) ? v : null };
        saveCalib();
        render(node);
      });
      btnPoint.addEventListener('click', () => {
        const raw = rawDistanceM(reports[node]);
        const decl = parseFloat(input.value);
        if (raw == null || !Number.isFinite(decl)) {
          log(`S${node}: falta la distancia FTM o la declarada`);
          return;
        }
        const c = calib[node] || {};
        c.points = [...(c.points || []), [raw, decl]];
        c.declared = decl;
        calib[node] = c;
        saveCalib();
        log(`S${node}: punto de calibración FTM ${raw.toFixed(2)} m -> ${decl.toFixed(2)} m`);
        render(node);
      });
      btnClear.addEventListener('click', () => {
        calib[node] = { declared: calib[node]?.declared ?? null, points: [] };
        saveCalib();
        render(node);
      });
      const after = Object.keys(rows).map(Number).filter((n) => n > node).sort((a, b) => a - b)[0];
      list.insertBefore(tr, after ? rows[after] : null);
      rows[node] = tr;
    }
    const s = nodeSummary(node);
    const fmt = (v, d) => (v == null ? '—' : Number(v).toFixed(d));
    const rtt = s.rtt_median_ps == null ? '—'
      : `${(s.rtt_median_ps / 1000).toFixed(1)} ±${((s.rtt_mad_ps || 0) / 1000).toFixed(1)} ns`;
    const skew = s.clock_skew_ppb == null ? '—' : `${(s.clock_skew_ppb / 1000).toFixed(2)} ppm`;
    tr.querySelector('.sync-ftm').textContent =
      `${s.ftm_status} · ${s.ftm_frames_valid}/${s.ftm_frames_total} tramas · RTT ${rtt} · ` +
      `deriva ${skew} · resid ${fmt(s.clock_residual_std_ns, 1)} ns`;
    const fit = s.calibration;
    const cal = s.distance_ftm_calibrated_m == null ? 'sin calibrar'
      : `${s.distance_ftm_calibrated_m.toFixed(2)} m calibrada (${fit.n_points} pts, ${fit.model === 'scale_only' ? 'escala' : 'escala+offset'})`;
    tr.querySelector('.sync-dist').textContent = `FTM cruda ${fmt(s.distance_ftm_raw_m, 2)} m → ${cal}`;
  };

  return {
    onReport(msg) {
      const node = msg.node | 0;
      if (node < 1) return;
      reports[node] = msg;
      render(node);
    },
  };
}
