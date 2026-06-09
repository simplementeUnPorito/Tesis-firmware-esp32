// slave_panel.js — per-slave config panel: PGA/VDAC, MAC, FIR/DC/notch,
// test/ver/send-all/latency-probe, statistics. Mirrors gui/slave_tab.py (SlaveTab — the *view*:
// builds DOM, dispatches CustomEvents instead of pyqtSignals; app.js owns the
// _on_* business logic, mirroring MainWindow). FIR/DC/notch controls land in
// Phase 4 alongside signal_proc.js; the serial debug-COM log group is desktop/
// USB-only (lists local PC ports) and has no phone-browser equivalent.

import * as cfg from './config.js?v=field-loop-11';

// Ganancia máxima del PGAvdac — valores más altos añaden demasiado ruido (mirrors SlaveTab._PGAVDAC_MAX_GAIN)
const PGAVDAC_MAX_GAIN = 8;

function vrefOutputV(vdacByte, pgaCode) {
  return vdacByte * cfg.VDAC_STEP * cfg.GAIN_CODES[pgaCode];
}

/** All representable (output_V, vdac_byte, pga_code) triples within the gain limit, sorted by output voltage. Mirrors SlaveTab._build_vref_values. */
function buildVrefValues() {
  const values = [];
  for (let pgaCode = 0; pgaCode < cfg.GAIN_CODES.length; pgaCode++) {
    const gain = cfg.GAIN_CODES[pgaCode];
    if (gain > PGAVDAC_MAX_GAIN) continue;
    for (let byte = cfg.VDAC_MIN; byte <= cfg.VDAC_MAX; byte++) {
      values.push([byte * cfg.VDAC_STEP * gain, byte, pgaCode]);
    }
  }
  values.sort((a, b) => (a[0] - b[0]) || (cfg.GAIN_CODES[a[2]] - cfg.GAIN_CODES[b[2]]) || (a[1] - b[1]));
  return values;
}

function cmpTuple(a, b) {
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

/** Closest representable (vdac_byte, pga_code) to targetV within the gain limit. Mirrors SlaveTab._calc_vdac. */
function calcVdac(targetV) {
  targetV = Math.max(0, targetV);
  let best = null; // [err, step, pgaCode, byte]
  for (let pcode = 0; pcode < cfg.GAIN_CODES.length; pcode++) {
    const gain = cfg.GAIN_CODES[pcode];
    if (gain > PGAVDAC_MAX_GAIN) continue;
    let byte = Math.round(targetV / (gain * cfg.VDAC_STEP));
    byte = Math.min(cfg.VDAC_MAX, Math.max(cfg.VDAC_MIN, byte));
    const actual = byte * cfg.VDAC_STEP * gain;
    const candidate = [Math.abs(actual - targetV), gain * cfg.VDAC_STEP, pcode, byte];
    if (best === null || cmpTuple(candidate, best) < 0) best = candidate;
  }
  return [best[3], best[2]]; // [byte, pgaCode]
}

/** Step to the next representable Target V above/below the current one. Mirrors SlaveTab._adjust_vref. */
function adjustVref(vdacByte, pgaCode, delta, vrefValues) {
  const current = vrefOutputV(vdacByte, pgaCode);
  const eps = 1e-12;
  let target;
  if (delta > 0) {
    const above = vrefValues.filter((it) => it[0] > current + eps);
    target = above.length ? above[0] : vrefValues[vrefValues.length - 1];
  } else {
    const below = vrefValues.filter((it) => it[0] < current - eps);
    target = below.length ? below[below.length - 1] : vrefValues[0];
  }
  return [target[1], target[2]]; // [byte, pgaCode]
}

// ── DOM helpers ───────────────────────────────────────────────────────────

function el(tag, className, text) {
  const e = document.createElement(tag);
  if (className) e.className = className;
  if (text !== undefined) e.textContent = text;
  return e;
}

function row(...children) {
  const r = el('div', 'row');
  for (const c of children) r.appendChild(c);
  return r;
}

function labeled(text, control) {
  const lbl = el('label');
  lbl.appendChild(document.createTextNode(text + ' '));
  lbl.appendChild(control);
  return lbl;
}

function dot(tooltip) {
  const d = el('span', 'dot');
  d.title = tooltip;
  return d;
}

function setDot(d, state, okTip, badTip, unkTip) {
  d.classList.remove('ok', 'bad');
  if (state === 1) { d.classList.add('ok'); d.title = okTip; }
  else if (state === 2) { d.classList.add('bad'); d.title = badTip; }
  else { d.title = unkTip; }
}

/**
 * Controls for one slave node (channels 1-3, displayed as Esclavo 1-3).
 * Dispatches CustomEvents the orchestrator (app.js) listens to and answers
 * with update*()/set*() calls — mirrors SlaveTab's pyqtSignal/slot pairs.
 *
 * Events: 'alias-changed' {alias}, 'vdac-changed' {byte},
 *         'pgavdac-changed' {code}, 'pga-changed' {code},
 *         'fir-apply' {cmd}, 'fir-remove', 'dc-remove-toggled' {enabled},
 *         'notch-toggled' {enabled}, 'notch-harm-changed' {harmonics},
 *         'test-requested', 'ver-requested', 'send-all-requested',
 *         'latency-requested', 'offset-changed' {offset}
 */
export class SlavePanel extends EventTarget {
  constructor(chIndex) {
    super();
    this.chIndex = chIndex;

    this._pgavdacCode = 0;
    this._vdacByte = 128;
    this._prevPgaCode = 0;
    this._gainTargetV = new Map();  // pgaCode -> last Target V (mirrors _gain_target_v)
    this._vrefValues = buildVrefValues();
    this._firActive = false;

    this.root = this._buildUi();
  }

  _dispatch(name, detail) {
    this.dispatchEvent(new CustomEvent(name, { detail }));
  }

  // ── UI construction ─────────────────────────────────────────────────────

  _buildUi() {
    const root = el('section', 'panel slave-panel');
    root.appendChild(el('h2', null, cfg.NODE_NAMES[this.chIndex]));

    // Identificación
    this._ddType = el('select');
    for (const t of cfg.SLAVE_TYPE_ORDER) this._ddType.appendChild(new Option(t, t));
    const typeDefaults = { 1: 'Hammer', 2: 'Geo1', 3: 'Geo2' };
    this._ddType.value = typeDefaults[this.chIndex] ?? 'Geo1';
    this._ddType.addEventListener('change', () => this._dispatch('alias-changed', this._ddType.value));
    this._lblMac = el('span', 'mac-label', 'MAC: —');
    root.appendChild(row(labeled('Tipo', this._ddType), this._lblMac));

    // Distancia al hammer (fuente sísmica)
    this._inpOffset = el('input');
    this._inpOffset.type = 'number';
    this._inpOffset.min = '0';
    this._inpOffset.step = '0.5';
    this._inpOffset.value = '0';
    this._inpOffset.placeholder = 'dist. al hammer';
    this._inpOffset.title = 'Distancia desde el hammer (fuente) hasta este receptor [m]';
    this._inpOffset.addEventListener('change', () => {
      this._dispatch('offset-changed', parseFloat(this._inpOffset.value) || 0);
    });
    root.appendChild(row(labeled('Offset m', this._inpOffset)));

    // VRef DC (VDAC)
    this._efTargetV = el('input');
    this._efTargetV.type = 'number';
    this._efTargetV.min = '0';
    this._efTargetV.max = String(cfg.VDAC_FULL_SCALE_V * Math.max(...cfg.GAIN_CODES));
    this._efTargetV.step = String(cfg.VDAC_STEP);
    this._efTargetV.value = '0.512';
    this._efTargetV.addEventListener('change', () => this._onTargetVChanged());
    const btnMinus = el('button', null, '−');
    const btnPlus = el('button', null, '+');
    btnMinus.addEventListener('click', () => this._adjustVref(-1));
    btnPlus.addEventListener('click', () => this._adjustVref(+1));
    this._dotVdac = dot('VDAC sin confirmar');
    root.appendChild(row(labeled('Target V', this._efTargetV), btnMinus, btnPlus, this._dotVdac));

    this._lblPgavdac = el('span', null, 'PGAvdac: 1x (auto)');
    root.appendChild(row(this._lblPgavdac));

    // PGA Gain
    this._ddPga = el('select');
    for (const name of cfg.GAIN_NAMES) this._ddPga.appendChild(new Option(name, name));
    this._ddPga.addEventListener('change', () => this._onPgaChanged());
    this._dotPga = dot('PGA sin confirmar');
    this._lblPgaActual = el('span', null, 'Current: 1x');
    root.appendChild(row(labeled('PGA', this._ddPga), this._dotPga, this._lblPgaActual));

    // FIR + DC removal (web subset: presets only, no scipy expressions)
    const firBox = el('div', 'filter-box');
    firBox.appendChild(el('div', 'subhead', 'FIR Filter'));

    this._ddFirType = el('select');
    for (const [value, label] of [
      ['lp', 'Low-pass'], ['hp', 'High-pass'], ['bp', 'Band-pass'], ['bs', 'Band-stop'],
    ]) {
      this._ddFirType.appendChild(new Option(label, value));
    }
    this._firF1 = el('input');
    this._firF1.type = 'number';
    this._firF1.min = '0';
    this._firF1.step = '1';
    this._firF1.value = '10';
    this._firF1.title = 'Cutoff 1, Hz';
    this._firF2 = el('input');
    this._firF2.type = 'number';
    this._firF2.min = '0';
    this._firF2.step = '1';
    this._firF2.value = '200';
    this._firF2.title = 'Cutoff 2, Hz';
    this._firTaps = el('input');
    this._firTaps.type = 'number';
    this._firTaps.min = '3';
    this._firTaps.max = '401';
    this._firTaps.step = '2';
    this._firTaps.value = '101';
    this._firTaps.title = 'Numero de taps';
    this._ddFirType.addEventListener('change', () => this._syncFirFields());
    this._btnApplyFir = el('button', null, 'Apply');
    this._btnApplyFir.addEventListener('click', () => this._onFirToggle());
    this._lblFirStatus = el('span', 'filter-status', 'No filter');
    firBox.appendChild(row(
      labeled('Tipo', this._ddFirType),
      labeled('F1 Hz', this._firF1),
      labeled('F2 Hz', this._firF2),
      labeled('Taps', this._firTaps),
      this._btnApplyFir,
    ));

    this._cbDcRemove = el('input');
    this._cbDcRemove.type = 'checkbox';
    this._cbDcRemove.addEventListener('change', () => {
      this._dispatch('dc-remove-toggled', { enabled: this._cbDcRemove.checked });
    });
    this._lblDcVal = el('span', 'dc-label', 'DC: --');
    firBox.appendChild(row(labeled('Remove DC', this._cbDcRemove), this._lblDcVal, this._lblFirStatus));
    root.appendChild(firBox);

    // Least-squares 50 Hz harmonic notch
    const notchBox = el('div', 'filter-box');
    notchBox.appendChild(el('div', 'subhead', '50 Hz Notch'));
    this._cbNotch = el('input');
    this._cbNotch.type = 'checkbox';
    this._cbNotch.addEventListener('change', () => {
      this._dispatch('notch-toggled', { enabled: this._cbNotch.checked });
    });
    this._spnNotchHarm = el('input');
    this._spnNotchHarm.type = 'number';
    this._spnNotchHarm.min = '1';
    this._spnNotchHarm.max = '5';
    this._spnNotchHarm.step = '1';
    this._spnNotchHarm.value = String(cfg.NOTCH_DEFAULT_HARM);
    this._spnNotchHarm.addEventListener('change', () => {
      this._dispatch('notch-harm-changed', { harmonics: parseInt(this._spnNotchHarm.value, 10) || 1 });
    });
    notchBox.appendChild(row(labeled('Enable', this._cbNotch), labeled('Harmonics', this._spnNotchHarm)));
    root.appendChild(notchBox);

    // Test / Ver / latency / send-all
    this._btnTest = el('button', null, 'Test');
    this._btnTest.title = 'Short directed debug burst';
    this._btnTest.addEventListener('click', () => this._dispatch('test-requested'));
    this._btnVer = el('button', null, 'Ver');
    this._btnVer.title = 'Single capture for live VDAC calibration';
    this._btnVer.addEventListener('click', () => this._dispatch('ver-requested'));
    this._btnLatency = el('button', null, 'Probe latency');
    this._btnLatency.addEventListener('click', () => this._dispatch('latency-requested'));
    root.appendChild(row(this._btnTest, this._btnVer, this._btnLatency));

    this._btnSendAll = el('button', null, 'Enviar configuracion');
    this._btnSendAll.title = 'Envia PGAvdac, VDAC y PGA de una sola vez';
    this._btnSendAll.addEventListener('click', () => this._dispatch('send-all-requested'));
    root.appendChild(row(this._btnSendAll));

    // Statistics
    const stats = el('div', 'slave-stats');
    this._lblStats = el('div', 'stat-line', 'Batches: 0  Samples: 0');
    this._lblLastVal = el('div', 'stat-line', 'Last: --');
    this._lblDrift = el('div', 'stat-line', 'Drift: --');
    this._lblLatency = el('div', 'stat-line', 'Latency: --');
    this._lblPsoc = el('div', 'stat-line', 'PSoC: ?');
    for (const l of [this._lblStats, this._lblLastVal, this._lblDrift, this._lblLatency, this._lblPsoc]) {
      stats.appendChild(l);
    }
    root.appendChild(stats);

    this._syncFirFields();
    return root;
  }

  // ── Internal handlers (mirror SlaveTab._on_*) ───────────────────────────

  _setVrefWidgets(vdacByte, pgaCode) {
    this._pgavdacCode = pgaCode;
    this._vdacByte = vdacByte;
    this._efTargetV.value = vrefOutputV(vdacByte, pgaCode).toFixed(4);
    const stepV = cfg.VDAC_STEP * cfg.GAIN_CODES[pgaCode];
    this._lblPgavdac.textContent = `PGAvdac: ${cfg.GAIN_NAMES[pgaCode]} (auto, step ${stepV.toFixed(3)} V)`;
  }

  _applyVrefSetting(vdacByte, pgaCode) {
    const oldByte = this._vdacByte;
    const oldPga = this._pgavdacCode;
    this._setVrefWidgets(vdacByte, pgaCode);
    if (pgaCode !== oldPga) this._dispatch('pgavdac-changed', pgaCode);
    if (vdacByte !== oldByte) this._dispatch('vdac-changed', vdacByte);
  }

  _adjustVref(delta) {
    const [byte, pcode] = adjustVref(this._vdacByte, this._pgavdacCode, delta, this._vrefValues);
    this._applyVrefSetting(byte, pcode);
  }

  _onTargetVChanged() {
    const target = parseFloat(this._efTargetV.value) || 0;
    const [byte, pcode] = calcVdac(target);
    this._applyVrefSetting(byte, pcode);
  }

  _onPgaChanged() {
    const index = this._ddPga.selectedIndex;
    this._gainTargetV.set(this._prevPgaCode, parseFloat(this._efTargetV.value) || 0);
    if (this._gainTargetV.has(index)) {
      const [byte, pcode] = calcVdac(this._gainTargetV.get(index));
      this._applyVrefSetting(byte, pcode);
    }
    this._prevPgaCode = index;
    this._lblPgaActual.textContent = `Current: ${cfg.GAIN_NAMES[index]}`;
    this._dispatch('pga-changed', index);
  }

  _syncFirFields() {
    const type = this._ddFirType.value;
    const needsF2 = type === 'bp' || type === 'bs';
    this._firF2.disabled = !needsF2;
  }

  _buildFirCommand() {
    let taps = parseInt(this._firTaps.value, 10) || 101;
    if (taps % 2 === 0) taps += 1;
    this._firTaps.value = String(taps);
    const type = this._ddFirType.value;
    const f1 = parseFloat(this._firF1.value);
    const f2 = parseFloat(this._firF2.value);
    if (type === 'lp' || type === 'hp') return `numtaps ${taps} ${type} ${f1}`;
    return `numtaps ${taps} ${type} ${f1} ${f2}`;
  }

  _onFirToggle() {
    if (this._firActive) {
      this._dispatch('fir-remove');
      return;
    }
    this._dispatch('fir-apply', { cmd: this._buildFirCommand() });
  }

  // ── Public API (mirror SlaveTab's slots, called by app.js) ──────────────

  setAlias(alias) {
    if ([...this._ddType.options].some((o) => o.value === alias)) this._ddType.value = alias;
  }

  setVisible(visible) {
    this.root.hidden = !visible;
  }

  setMac(mac) {
    this._lblMac.textContent = `MAC: ${mac.toUpperCase()}`;
  }

  setConnected(connected) {
    this._btnTest.disabled = !connected;
    this._btnVer.disabled = !connected;
    this._btnLatency.disabled = !connected;
    this._btnSendAll.disabled = !connected;
    if (!connected) this.setPgaLock(0);
  }

  setFirPreset(type, f1, f2, taps) {
    if ([...this._ddFirType.options].some((o) => o.value === type)) this._ddFirType.value = type;
    if (Number.isFinite(f1)) this._firF1.value = String(f1);
    if (Number.isFinite(f2)) this._firF2.value = String(f2);
    if (Number.isFinite(taps)) this._firTaps.value = String(taps);
    this._syncFirFields();
  }

  setFirStatus(status) {
    this._lblFirStatus.textContent = status;
    this._firActive = status !== 'No filter';
    this._btnApplyFir.textContent = this._firActive ? 'Remove' : 'Apply';
  }

  setDcRemove(enabled) {
    this._cbDcRemove.checked = !!enabled;
  }

  setDcValue(v) {
    this._lblDcVal.textContent = (v === null || v === undefined) ? 'DC: --' : `DC: ${v.toFixed(5)} V`;
  }

  setNotchEnabled(enabled) {
    this._cbNotch.checked = !!enabled;
  }

  setNotchHarmonics(n) {
    const val = Math.max(1, Math.min(5, parseInt(n, 10) || cfg.NOTCH_DEFAULT_HARM));
    this._spnNotchHarm.value = String(val);
  }

  /** vdac_byte: update the VDAC widgets without re-emitting (avoids feedback loop). */
  setVdac(byteVal) {
    this._setVrefWidgets(byteVal, this._pgavdacCode);
  }

  setPgavdac(code) {
    if (code >= 0 && code < cfg.GAIN_CODES.length) this._setVrefWidgets(this._vdacByte, code);
  }

  setPga(code) {
    if (code < 0 || code >= cfg.GAIN_NAMES.length) return;
    this._ddPga.selectedIndex = code;
    this._prevPgaCode = code;
    this._lblPgaActual.textContent = `Current: ${cfg.GAIN_NAMES[code]}`;
  }

  /** state: 0=unknown, 1=locked, 2=pending/fail. */
  setPgaLock(state) {
    setDot(this._dotPga, state, 'PGA confirmado por PSoC', 'PGA pendiente o sin confirmacion', 'PGA sin confirmar');
  }

  /** state: 0=unknown, 1=ok, 2=pending. */
  setVdacLock(state) {
    setDot(this._dotVdac, state, 'VDAC confirmado por PSoC', 'VDAC pendiente', 'VDAC sin confirmar');
  }

  updateStats(batchCount, totalSamples, lastVal, driftStr, latencyStr, psocOk) {
    this._lblStats.textContent = `Batches: ${batchCount}  Samples: ${totalSamples}`;
    this._lblLastVal.textContent = (lastVal === null || lastVal === undefined)
      ? 'Last: --' : `Last: ${lastVal.toFixed(6)} V`;
    this._lblDrift.textContent = `Drift: ${driftStr}`;
    this._lblLatency.textContent = `Latency: ${latencyStr}`;
    this._lblPsoc.textContent = (psocOk === null || psocOk === undefined)
      ? 'PSoC: ?' : (psocOk ? 'PSoC: DETECTED' : 'PSoC: not detected');
  }

  get vdacByte() { return this._vdacByte; }
  get firPreset() {
    return {
      type: this._ddFirType.value,
      f1: parseFloat(this._firF1.value),
      f2: parseFloat(this._firF2.value),
      taps: parseInt(this._firTaps.value, 10) || 101,
    };
  }

  setOffset(v) {
    this._inpOffset.value = String(Number.isFinite(v) ? v : 0);
  }

  getGainTargetV() { return new Map(this._gainTargetV); }
  setGainTargetV(map) { for (const [k, v] of map) this._gainTargetV.set(k, v); }
}
