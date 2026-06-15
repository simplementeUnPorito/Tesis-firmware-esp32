// slave_panel.js - per-slave config panel: PGA, calibracion PSoC, MAC, FIR/DC,
// test/ver/latency, statistics. Mirrors gui/slave_tab.py at the view layer:
// this module builds DOM and dispatches CustomEvents; app.js owns orchestration.

import * as cfg from './config.js?v=field-loop-20';

// -- DOM helpers -------------------------------------------------------------

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
  lbl.appendChild(document.createTextNode(`${text} `));
  lbl.appendChild(control);
  return lbl;
}

function dot(tooltip) {
  const d = el('span', 'dot');
  d.title = tooltip;
  return d;
}

function setDot(d, state, okTip, badTip, unkTip, busyTip) {
  d.classList.remove('ok', 'bad', 'busy');
  if (state === 1) { d.classList.add('ok'); d.title = okTip; }
  else if (state === 2) { d.classList.add('bad'); d.title = badTip; }
  else if (state === 3) { d.classList.add('busy'); d.title = busyTip ?? 'Calibrando...'; }
  else { d.title = unkTip; }
}

/**
 * Controls for one slave node (channels 1-3, displayed as Esclavo 1-3).
 *
 * Events: 'alias-changed' {alias}, 'pga-changed' {code},
 *         'fir-apply' {cmd}, 'fir-remove',
 *         'dc-remove-toggled' {enabled}, 'test-requested', 'ver-requested',
 *         'send-all-requested', 'latency-requested', 'offset-changed' {offset}
 */
export class SlavePanel extends EventTarget {
  constructor(chIndex) {
    super();
    this.chIndex = chIndex;
    this._firActive = false;

    this.root = this._buildUi();
  }

  _dispatch(name, detail) {
    this.dispatchEvent(new CustomEvent(name, { detail }));
  }

  // -- UI construction -------------------------------------------------------

  _buildUi() {
    const root = el('section', 'panel slave-panel');
    root.appendChild(el('h2', null, cfg.NODE_NAMES[this.chIndex]));

    this._ddType = el('select');
    for (const t of cfg.SLAVE_TYPE_ORDER) this._ddType.appendChild(new Option(t, t));
    const typeDefaults = { 1: 'Hammer', 2: 'Geo1', 3: 'Geo2' };
    this._ddType.value = typeDefaults[this.chIndex] ?? 'Geo1';
    this._ddType.addEventListener('change', () => this._dispatch('alias-changed', this._ddType.value));
    this._lblMac = el('span', 'mac-label', 'MAC: --');
    root.appendChild(row(labeled('Tipo', this._ddType), this._lblMac));

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

    this._ddPga = el('select');
    for (const name of cfg.GAIN_NAMES) this._ddPga.appendChild(new Option(name, name));
    this._ddPga.addEventListener('change', () => this._onPgaChanged());
    this._dotPga = dot('PGA sin confirmar');
    this._lblPgaActual = el('span', null, 'Current: 1x');
    root.appendChild(row(labeled('PGA', this._ddPga), this._dotPga, this._lblPgaActual));

    this._dotCal = null;

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

    this._btnTest = el('button', null, 'Test');
    this._btnTest.title = 'Short directed debug burst';
    this._btnTest.addEventListener('click', () => this._dispatch('test-requested'));
    this._btnVer = el('button', null, 'Ver');
    this._btnVer.title = 'Single capture';
    this._btnVer.addEventListener('click', () => this._dispatch('ver-requested'));
    this._btnLatency = el('button', null, 'Probe latency');
    this._btnLatency.addEventListener('click', () => this._dispatch('latency-requested'));
    root.appendChild(row(this._btnTest, this._btnVer, this._btnLatency));

    this._btnSendAll = el('button', null, 'Enviar Config');
    this._btnSendAll.title = 'Envia la configuracion de PGA al PSoC';
    this._btnSendAll.addEventListener('click', () => this._dispatch('send-all-requested'));
    root.appendChild(row(this._btnSendAll));

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

  // -- Internal handlers -----------------------------------------------------

  _onPgaChanged() {
    const index = this._ddPga.selectedIndex;
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

  // -- Public API ------------------------------------------------------------

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
    if (!connected) {
      this.setPgaLock(0);
      this.setCalibrationLock(0);
    }
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

  setPga(code) {
    if (code < 0 || code >= cfg.GAIN_NAMES.length) return;
    this._ddPga.selectedIndex = code;
    this._lblPgaActual.textContent = `Current: ${cfg.GAIN_NAMES[code]}`;
  }

  /** state: 0=unknown, 1=locked, 2=pending/fail. */
  setPgaLock(state) {
    setDot(this._dotPga, state, 'PGA confirmado por PSoC', 'PGA pendiente o sin confirmacion', 'PGA sin confirmar');
  }

  /** state: 0=unknown, 1=ok, 2=pending/fail, 3=calibrando (en curso). */
  setCalibrationLock(state) {
    if (!this._dotCal) return;
    setDot(this._dotCal, state, 'Calibracion confirmada por PSoC', 'Calibracion pendiente o fallida',
           'Calibracion sin ejecutar', 'Calibrando... (puede tardar hasta ~3 min)');
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
}
