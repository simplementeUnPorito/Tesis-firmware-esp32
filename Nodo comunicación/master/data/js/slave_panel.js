// slave_panel.js - per-slave config panel: PGA, calibracion PSoC, MAC, FIR/DC,
// test/ver/latency, statistics. Mirrors gui/slave_tab.py at the view layer:
// this module builds DOM and dispatches CustomEvents; app.js owns orchestration.

import * as cfg from './config.js?v=field-study-19';

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

function vdacMv(value) {
  return Number.isFinite(value) ? Math.round(value * cfg.VDAC_MV_PER_CODE) : null;
}

function fmtSignedMv(value) {
  return Number.isFinite(value) ? `${value >= 0 ? '+' : ''}${Math.round(value)}mV` : '--';
}

function fmtSignedPct(value) {
  return Number.isFinite(value) ? `${value >= 0 ? '+' : ''}${value.toFixed(2)}%` : '--';
}

/**
 * Controls for one slave node (channels 1-3, displayed as Esclavo 1-3).
 *
 * Events: 'pga-changed' {code}, 'adc-config-changed' {code},
 *         'fir-apply' {cmd}, 'fir-remove',
 *         'line-notch-changed' {enabled, nHarm},
 *         'dc-remove-toggled' {enabled}, 'test-requested', 'ver-requested',
 *         'send-all-requested', 'calibrate-requested', 'latency-requested',
 *         'offset-changed' {offset}, 'y-offset-changed' {offsetMv}
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

    this._slaveKind = 'unknown';
    this._lblMac = el('span', 'mac-label', 'MAC: --');
    this._lblRssi = el('span', 'rssi-label', 'RSSI: --');
    this._lblRssi.title = 'Intensidad de señal ESP-NOW esclavo <-> maestro (se pausa durante captura)';
    root.appendChild(row(this._lblMac, this._lblRssi));

    this._inpOffset = el('input');
    this._inpOffset.type = 'number';
    this._inpOffset.min = '0';
    this._inpOffset.step = '0.5';
    this._inpOffset.value = '0';
    this._inpOffset.placeholder = 'dist. al hammer';
    this._inpOffset.title = 'Distancia desde el hammer (fuente) hasta este receptor [m]';
    const emitOffsetChanged = () => {
      this._dispatch('offset-changed', parseFloat(this._inpOffset.value) || 0);
    };
    this._inpOffset.addEventListener('input', emitOffsetChanged);
    this._inpOffset.addEventListener('change', emitOffsetChanged);
    this._rowOffset = row(labeled('Offset m', this._inpOffset));
    root.appendChild(this._rowOffset);

    this._cbInvert = el('input');
    this._cbInvert.type = 'checkbox';
    this._cbInvert.title = 'Invertir señal (×−1) para display y export';
    this._cbInvert.addEventListener('change', () => {
      this._dispatch('invert-toggled', { enabled: this._cbInvert.checked });
    });
    root.appendChild(row(labeled('Invertir señal', this._cbInvert)));

    this._ddPga = el('select');
    for (const name of cfg.GAIN_NAMES) this._ddPga.appendChild(new Option(name, name));
    this._ddPga.addEventListener('change', () => this._onPgaChanged());
    this._dotPga = dot('PGA sin confirmar');
    this._lblPgaActual = el('span', null, 'Current: 1x');
    this._btnSendAll = el('button', null, 'resend');
    this._btnSendAll.title = 'Reenvia la configuracion de PGA y rango ADC al PSoC';
    this._btnSendAll.addEventListener('click', () => this._dispatch('send-all-requested'));
    root.appendChild(row(labeled('PGA', this._ddPga), this._btnSendAll, this._dotPga, this._lblPgaActual));

    this._ddAdcCfg = el('select');
    for (const adcCfg of cfg.ADC_CONFIGS) {
      this._ddAdcCfg.appendChild(new Option(adcCfg.label, String(adcCfg.code)));
    }
    this._ddAdcCfg.addEventListener('change', () => this._onAdcConfigChanged());
    this._dotAdcCfg = dot('Rango ADC sin confirmar');
    this._lblAdcCfgActual = el('span', null, `Current: ${cfg.ADC_CONFIGS[0].label}`);
    root.appendChild(row(labeled('Rango ADC', this._ddAdcCfg), this._dotAdcCfg, this._lblAdcCfgActual));

    /* Decimación: control GLOBAL en la barra superior desde 2026-07-11 (un solo
     * factor para todos los nodos — mezclar Fs entre nodos rompe el análisis).
     * El dot de confirmación por nodo quedó junto al de Rango ADC arriba. */
    this._dotDecim = dot('Decimación sin confirmar');
    const decimRow = row(el('span', null, 'Decimación (global)'), this._dotDecim);
    root.appendChild(decimRow);

    /* SD (vive en el PSoC, solo GEO): la fila entera queda OCULTA salvo que el
     * PSoC reporte sd_present=1 — en un HAMMER o sin tarjeta no aparece nada. */
    this._sdPresent = false;
    this._cbSdEnable = el('input');
    this._cbSdEnable.type = 'checkbox';
    this._cbSdEnable.disabled = true;
    this._cbSdEnable.title = 'Grabar la captura en la SD del PSoC (permite ventanas más largas que la RAM)';
    this._cbSdEnable.addEventListener('change', () => {
      this._dispatch('sd-enable-changed', { enabled: this._cbSdEnable.checked });
    });
    this._dotSd = dot('SD no detectada');
    this._lblSdActual = el('span', null, 'SD: sin detectar');
    this._rowSd = row(labeled('SD', this._cbSdEnable), this._dotSd, this._lblSdActual);
    this._rowSd.style.display = 'none';
    root.appendChild(this._rowSd);

    this._dotCal = dot('Calibracion sin ejecutar');
    this._btnCalibrate = el('button', null, 'Calibrar');
    this._btnCalibrate.title = 'Calibrar PSoC';
    this._btnCalibrate.addEventListener('click', () => this._dispatch('calibrate-requested'));
    root.appendChild(row(el('span', null, 'Calibracion'), this._dotCal, this._btnCalibrate));

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
      labeled('Filtro', this._ddFirType),
      labeled('F1 Hz', this._firF1),
      labeled('F2 Hz', this._firF2),
      labeled('Taps', this._firTaps),
      this._btnApplyFir,
    ));

    firBox.appendChild(row(this._lblFirStatus));

    this._cbLineNotch = el('input');
    this._cbLineNotch.type = 'checkbox';
    this._cbLineNotch.title = 'Cancelar ruido de linea por ajuste de armónicos sobre la captura completa';
    this._notchHarm = el('input');
    this._notchHarm.type = 'number';
    this._notchHarm.min = '1';
    this._notchHarm.max = String(cfg.LINE_NOTCH_MAX_HARM);
    this._notchHarm.step = '1';
    this._notchHarm.value = String(cfg.LINE_NOTCH_DEFAULT_HARM);
    this._notchHarm.className = 'small-number';
    this._notchHarm.title = 'Cantidad de armonicos 50*N a cancelar';
    const emitNotchChanged = () => {
      this._dispatch('line-notch-changed', {
        enabled: this._cbLineNotch.checked,
        nHarm: parseInt(this._notchHarm.value, 10) || cfg.LINE_NOTCH_DEFAULT_HARM,
      });
    };
    this._cbLineNotch.addEventListener('change', emitNotchChanged);
    this._notchHarm.addEventListener('change', emitNotchChanged);
    firBox.appendChild(row(
      labeled('Sin ruido de linea', this._cbLineNotch),
      labeled('N', this._notchHarm),
    ));
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

    /* ── Controles hardware FIR / EEPROM / identificación ──────────────────── */
    const hwBox = el('div', 'filter-box');
    hwBox.appendChild(el('div', 'subhead', 'Hardware PSoC'));

    this._cbFirHw = el('input');
    this._cbFirHw.type = 'checkbox';
    this._cbFirHw.title = 'Enviar señal filtrada por el FIR hardware del PSoC en vez del ADC crudo';
    this._cbFirHw.addEventListener('change', () => {
      this._dispatch('fir-hw-toggled', { mode: this._cbFirHw.checked ? 1 : 0 });
    });
    this._lblFirHw = el('span', null, 'FIR hardware');
    hwBox.appendChild(row(labeled('', this._cbFirHw), this._lblFirHw));

    this._btnSaveEeprom = el('button', null, 'Guardar EEPROM');
    this._btnSaveEeprom.title = 'Guarda los valores VDAC de calibración actuales en la EEPROM del PSoC';
    this._btnSaveEeprom.addEventListener('click', () => this._dispatch('save-eeprom-requested'));
    this._dotEeprom = dot('EEPROM sin guardar');

    this._btnBlinkLed = el('button', null, 'Titilar LED');
    this._btnBlinkLed.title = 'Hace titilar el LED del ESP esclavo para identificarlo físicamente';
    this._btnBlinkLed.addEventListener('click', () => this._dispatch('blink-led-requested'));

    hwBox.appendChild(row(this._btnSaveEeprom, this._dotEeprom));
    hwBox.appendChild(row(this._btnBlinkLed));
    root.appendChild(hwBox);

    const stats = el('div', 'slave-stats');
    this._lblStats = el('div', 'stat-line', 'Batches: 0  Samples: 0');
    this._lblLastVal = el('div', 'stat-line', 'Last: --');
    this._lblLatency = el('div', 'stat-line', 'Latency: --');
    this._lblPsoc = el('div', 'stat-line', 'PSoC: ?');
    this._lblVdac = el('div', 'stat-line', 'VDACs: --');
    for (const l of [this._lblStats, this._lblLastVal, this._lblLatency, this._lblPsoc, this._lblVdac]) {
      stats.appendChild(l);
    }
    root.appendChild(stats);

    this._syncFirFields();
    this._syncTypeLayout();
    return root;
  }

  // -- Internal handlers -----------------------------------------------------

  _syncTypeLayout() {
    const isHammer = this._slaveKind === 'hammer';
    this._rowOffset.hidden = isHammer;
    this._rowOffset.style.display = isHammer ? 'none' : '';
    if (isHammer) {
      this._inpOffset.value = '0';
    }
  }

  _onPgaChanged() {
    const index = this._ddPga.selectedIndex;
    this._lblPgaActual.textContent = `Current: ${cfg.GAIN_NAMES[index]}`;
    this._dispatch('pga-changed', index);
  }

  _onAdcConfigChanged() {
    const code = parseInt(this._ddAdcCfg.value, 10) || 1;
    const adcCfg = cfg.ADC_CONFIGS.find((item) => item.code === code) || cfg.ADC_CONFIGS[0];
    this._lblAdcCfgActual.textContent = `Current: ${adcCfg.label}`;
    this._dispatch('adc-config-changed', { code: adcCfg.code });
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
    const name = String(alias || '').trim();
    if (name === 'Hammer') this._slaveKind = 'hammer';
    else if (/^Geo\d*$/i.test(name)) this._slaveKind = 'geo';
    else this._slaveKind = 'unknown';
    this._syncTypeLayout();
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
    this._ddPga.disabled = !connected;
    this._ddAdcCfg.disabled = !connected;
    this._cbSdEnable.disabled = !connected || !this._sdPresent;
    this._btnCalibrate.disabled = !connected;
    this._btnSaveEeprom.disabled = !connected;
    this._btnBlinkLed.disabled = !connected;
    this._cbFirHw.disabled = !connected;
    if (!connected) {
      this.setPgaLock(0);
      this.setAdcConfigLock(0);
      this.setDecimationLock(0);
      this.setCalibrationLock(0);
      this.setSdPresent(false, false);
      this.setSdLock(0);
    }
  }

  /** state: 0=unknown, 1=ok (guardado), 2=fail. */
  setEepromLock(state) {
    setDot(this._dotEeprom, state, 'EEPROM guardada', 'Error al guardar EEPROM', 'EEPROM sin guardar');
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

  setDcRemove(_enabled) {}

  setDcValue(_v) {}

  setLineNotch(enabled, nHarm) {
    this._cbLineNotch.checked = !!enabled;
    const v = Number.isFinite(nHarm) ? nHarm : cfg.LINE_NOTCH_DEFAULT_HARM;
    this._notchHarm.value = String(Math.min(cfg.LINE_NOTCH_MAX_HARM, Math.max(1, Math.round(v))));
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

  setAdcConfig(code) {
    const adcCfg = cfg.ADC_CONFIGS.find((item) => item.code === code) || cfg.ADC_CONFIGS[0];
    this._ddAdcCfg.value = String(adcCfg.code);
    this._lblAdcCfgActual.textContent = `Current: ${adcCfg.label}`;
  }

  /** rssi: dBm (int) o null si no hay dato reciente (esclavo mudo o en captura). */
  setRssi(rssi) {
    if (!this._lblRssi) return;
    this._lblRssi.textContent = Number.isFinite(rssi) ? `RSSI: ${rssi} dBm` : 'RSSI: --';
  }

  /** state: 0=unknown, 1=locked, 2=pending/fail. */
  setAdcConfigLock(state) {
    setDot(this._dotAdcCfg, state, 'Rango ADC confirmado por PSoC',
           'Rango ADC pendiente o sin confirmacion', 'Rango ADC sin confirmar');
  }

  setDecimation(_factor) {
    /* El valor vive en el control global (app.js); acá solo queda el dot. */
  }

  /** state: 0=unknown, 1=locked, 2=pending/fail. */
  setDecimationLock(state) {
    setDot(this._dotDecim, state, 'Decimación confirmada por PSoC',
           'Decimación pendiente o sin confirmacion', 'Decimación sin confirmar');
  }

  /** Llamado al recibir HELLO sub 0x07 (sd_present) o al desconectar el nodo.
   * Sin SD detectada la fila entera desaparece del panel. */
  setSdPresent(present, enabled) {
    this._sdPresent = !!present;
    this._rowSd.style.display = this._sdPresent ? '' : 'none';
    this._cbSdEnable.disabled = !this._sdPresent || this._btnTest.disabled; /* btnTest.disabled ~= !connected */
    this._lblSdActual.textContent = this._sdPresent
      ? `SD: detectada (${enabled ? 'ON' : 'OFF'})`
      : 'SD: sin detectar';
    if (!this._sdPresent) {
      this._cbSdEnable.checked = false;
      this.setSdLock(0);
    } else {
      this._cbSdEnable.checked = !!enabled;
    }
  }

  setSdEnabled(enabled) {
    this._cbSdEnable.checked = !!enabled;
    this._lblSdActual.textContent = this._sdPresent
      ? `SD: detectada (${enabled ? 'ON' : 'OFF'})`
      : 'SD: sin detectar';
  }

  /** state: 0=unknown, 1=locked, 2=pending/fail. */
  setSdLock(state) {
    setDot(this._dotSd, state, 'SD confirmada por el esclavo',
           'SD pendiente o sin confirmacion', 'SD no detectada');
  }

  /** state: 0=unknown, 1=ok, 2=pending/fail, 3=calibrando (en curso). */
  setCalibrationLock(state, detail) {
    if (!this._dotCal) return;
    const busyTip = detail ? `Calibrando... ${detail}` : 'Calibrando... (puede tardar hasta ~4 min)';
    setDot(this._dotCal, state, 'Calibracion confirmada por PSoC', 'Calibracion pendiente o fallida',
           'Calibracion sin ejecutar', busyTip);
  }

  setVdac(vdacByte) {
    if (vdacByte === null || vdacByte === undefined) {
      this._lblVdac.textContent = 'VDACs: --';
    } else {
      const pct = ((vdacByte / 255) * 100).toFixed(1);
      this._lblVdac.textContent = `VDACs: legacy ${vdacByte} (${vdacMv(vdacByte)}mV, ${pct}%)`;
    }
  }

  setVdacs(values, hwClass = 0xFF, legacyVdac = null, details = null) {
    const labels = hwClass === 1 ? cfg.CAL_VDAC_LABELS_HAMMER : cfg.CAL_VDAC_LABELS_GEO;
    const items = [];
    for (let i = 0; i < labels.length; i++) {
      const value = values && values[i];
      if (Number.isFinite(value)) {
        const detail = details && details[i];
        const mv = vdacMv(value);
        let text = `${labels[i]} ${value}/${mv}mV`;
        if (detail && Number.isFinite(detail.error_mV)) {
          const targetText = Number.isFinite(detail.target_mV) ? ` vs ${Math.round(detail.target_mV)}mV` : '';
          text += ` err ${fmtSignedMv(detail.error_mV)}${targetText} (${fmtSignedPct(detail.error_pct)})`;
        }
        items.push(text);
      }
    }
    if (items.length) {
      this._lblVdac.textContent = `VDACs: ${items.join(' · ')}`;
    } else {
      this.setVdac(legacyVdac);
    }
  }

  updateStats(batchCount, totalSamples, lastVal, _driftStr, latencyStr, psocOk) {
    this._lblStats.textContent = `Batches: ${batchCount}  Samples: ${totalSamples}`;
    this._lblLastVal.textContent = (lastVal === null || lastVal === undefined)
      ? 'Last: --' : `Last: ${lastVal.toFixed(6)} V`;
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

  setYOffset(_mv) {}

  setInvertSignal(enabled) {
    this._cbInvert.checked = !!enabled;
  }

  /** Sync panel layout from the slave type reported by firmware. */
  setSlaveType(typeName) {
    this.setAlias(typeName);
  }

  /** Label displayed in the panel header (replaces generic "Esclavo N" with geo alias). */
  setDisplayName(name) {
    const h2 = this.root.querySelector('h2');
    if (h2) h2.textContent = name;
  }
}
