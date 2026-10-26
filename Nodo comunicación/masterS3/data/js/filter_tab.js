// Pestaña Filtros. Por defecto el martillo y todos los geófonos comparten los
// FIR; se puede separar explícitamente, con una advertencia temporal visible.

const STORAGE_PREFIX = 'geophone_scope_web.filter_group_';
const COMMON_FIR_KEY = 'geophone_scope_web.same_fir_geo_hammer_v1';
const FIR_CHANGES = new Set(['fir', 'firhw', 'firpsoc']);
export const FILTER_GROUPS = [
  ['geo', 'GEO (todos los geófonos)'],
  ['hammer', 'HAMMER'],
];
const CLEANING = [
  ['i2c', 'Eliminar ruido I2C'],
  ['sd', 'Eliminar ruido SD'],
  ['radio', 'Eliminar ruido transmisión'],
  ['beacon', 'Eliminar ruido beacon'],
];

export function defaultGroupState() {
  return {
    firActive: false,
    firType: 'lp', f1: 10, f2: 200, taps: 101,
    notchEnabled: false, notchHarm: 3,
    clean: { i2c: false, sd: false, radio: false, beacon: false },
    firHw: 0,
    psocFir: { active: false, type: 'lp', f1: 200, f2: 400 },
  };
}

export function loadGroupState(group) {
  const base = defaultGroupState();
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_PREFIX + group) || 'null');
    if (saved && typeof saved === 'object') {
      return { ...base, ...saved, clean: { ...base.clean, ...(saved.clean || {}) },
        psocFir: { ...base.psocFir, ...(saved.psocFir || {}) } };
    }
  } catch (_) { /* sin almacenamiento: valores por defecto */ }
  return base;
}

function saveGroupState(group, state) {
  try { localStorage.setItem(STORAGE_PREFIX + group, JSON.stringify(state)); } catch (_) {}
}

function loadCommonFir() {
  try { return localStorage.getItem(COMMON_FIR_KEY) !== 'false'; } catch (_) { return true; }
}

function saveCommonFir(value) {
  try { localStorage.setItem(COMMON_FIR_KEY, value ? 'true' : 'false'); } catch (_) {}
}

function copyFirState(source, target) {
  target.firActive = !!source.firActive;
  target.firType = source.firType;
  target.f1 = source.f1;
  target.f2 = source.f2;
  target.taps = source.taps;
  target.firHw = source.firHw ? 1 : 0;
  if (!target.psocFir) target.psocFir = {};
  Object.assign(target.psocFir, source.psocFir);
}

export function firCommand(state) {
  let taps = parseInt(state.taps, 10) || 101;
  if (taps % 2 === 0) taps += 1;
  if (state.firType === 'lp' || state.firType === 'hp') return `numtaps ${taps} ${state.firType} ${state.f1}`;
  return `numtaps ${taps} ${state.firType} ${state.f1} ${state.f2}`;
}

function labeled(text, input) {
  const l = document.createElement('label');
  l.append(text, ' ', input);
  return l;
}

/**
 * Monta la pestaña. onChange(group, state, what) se llama en cada cambio;
 * what es 'fir' | 'notch' | 'clean' | 'firhw'. onSignatures(file) recibe el
 * archivo de firmas elegido.
 */
export function mountFilterTab(container, { onChange, onSignatures, maxHarm = 12 }) {
  const states = Object.fromEntries(FILTER_GROUPS.map(([group]) => [group, loadGroupState(group)]));
  let sameFir = loadCommonFir();
  if (sameFir) {
    copyFirState(states.geo, states.hammer);
    saveGroupState('hammer', states.hammer);
  }
  const statusEls = {};
  const refreshByGroup = {};

  const commonBox = document.createElement('fieldset');
  commonBox.className = 'panel';
  commonBox.innerHTML = '<legend>Coherencia temporal</legend>';
  const common = document.createElement('input');
  common.type = 'checkbox';
  common.checked = sameFir;
  const warning = document.createElement('div');
  warning.className = 'filter-status';
  warning.textContent = 'AVISO: FIR distintos corren los tiempos de arribo entre martillo y geófonos.';
  warning.hidden = sameFir;
  commonBox.append(labeled('Mismo FIR para martillo y geófonos', common), warning);
  container.append(commonBox);

  const emitChange = (group, what) => {
    if (sameFir && FIR_CHANGES.has(what)) {
      const other = group === 'geo' ? 'hammer' : 'geo';
      copyFirState(states[group], states[other]);
      saveGroupState(other, states[other]);
      if (refreshByGroup[other]) refreshByGroup[other]();
    }
    onChange(group, states[group], what);
  };

  for (const [group, title] of FILTER_GROUPS) {
    const st = states[group];
    const box = document.createElement('fieldset');
    box.className = 'panel filter-group';
    box.innerHTML = `<legend>${title}</legend>`;

    // FIR web (sobre la captura completa, fase cero)
    const type = document.createElement('select');
    for (const [v, t] of [['lp', 'Pasa bajos'], ['hp', 'Pasa altos'], ['bp', 'Pasa banda'], ['bs', 'Rechaza banda']]) {
      type.appendChild(new Option(t, v));
    }
    type.value = st.firType;
    const num = (value, step, min, max) => {
      const i = document.createElement('input');
      i.type = 'number'; i.step = String(step); i.min = String(min);
      if (max != null) i.max = String(max);
      i.value = String(value); i.className = 'small-number';
      return i;
    };
    const f1 = num(st.f1, 1, 0);
    const f2 = num(st.f2, 1, 0);
    const taps = num(st.taps, 2, 3, 401);
    const apply = document.createElement('button');
    apply.type = 'button';
    const status = document.createElement('span');
    status.className = 'filter-status';
    statusEls[group] = status;
    const refreshFir = () => {
      apply.textContent = st.firActive ? 'Quitar' : 'Aplicar';
      status.textContent = st.firActive ? `FIR web: ${firCommand(st)}` : 'FIR web: ninguno';
      f2.disabled = st.firType === 'lp' || st.firType === 'hp';
    };
    const readFir = () => {
      st.firType = type.value;
      st.f1 = parseFloat(f1.value) || 0;
      st.f2 = parseFloat(f2.value) || 0;
      st.taps = parseInt(taps.value, 10) || 101;
    };
    type.addEventListener('change', () => { readFir(); refreshFir(); saveGroupState(group, st); });
    apply.addEventListener('click', () => {
      readFir();
      st.firActive = !st.firActive;
      saveGroupState(group, st);
      refreshFir();
      emitChange(group, 'fir');
    });
    const firRow = document.createElement('div');
    firRow.className = 'row';
    firRow.append(labeled('Filtro', type), labeled('F1 Hz', f1), labeled('F2 Hz', f2), labeled('Taps', taps), apply);
    box.append(firRow, status);

    // Ruido de linea
    const notch = document.createElement('input');
    notch.type = 'checkbox'; notch.checked = !!st.notchEnabled;
    const harm = num(st.notchHarm, 1, 1, maxHarm);
    const emitNotch = () => {
      st.notchEnabled = notch.checked;
      st.notchHarm = parseInt(harm.value, 10) || 3;
      saveGroupState(group, st);
      onChange(group, st, 'notch');
    };
    notch.addEventListener('change', emitNotch);
    harm.addEventListener('change', emitNotch);
    const notchRow = document.createElement('div');
    notchRow.className = 'row';
    notchRow.append(labeled('Eliminar ruido de línea', notch), labeled('N', harm));
    box.append(notchRow);

    // Limpieza por marcas
    const cleanRow = document.createElement('div');
    cleanRow.className = 'row';
    for (const [kind, text] of CLEANING) {
      const cb = document.createElement('input');
      cb.type = 'checkbox'; cb.checked = !!st.clean[kind];
      cb.addEventListener('change', () => {
        st.clean[kind] = cb.checked;
        saveGroupState(group, st);
        onChange(group, st, 'clean');
      });
      cleanRow.append(labeled(text, cb));
    }
    box.append(cleanRow);

    // FIR del PSoC (filter block): manda la salida filtrada en vez del ADC crudo
    const firHw = document.createElement('input');
    firHw.type = 'checkbox'; firHw.checked = !!st.firHw;
    firHw.addEventListener('change', () => {
      st.firHw = firHw.checked ? 1 : 0;
      saveGroupState(group, st);
      emitChange(group, 'firhw');
    });
    const hwRow = document.createElement('div');
    hwRow.className = 'row';
    hwRow.append(labeled('Mandar la salida del FIR del PSoC', firHw));
    box.append(hwRow);

    // Coeficientes del bloque Filter del PSoC (128 taps, a la tasa del ADC)
    const pf = st.psocFir;
    const pType = document.createElement('select');
    for (const [v, t] of [['lp', 'Pasa bajos'], ['hp', 'Pasa altos'], ['bp', 'Pasa banda'], ['bs', 'Rechaza banda']]) {
      pType.appendChild(new Option(t, v));
    }
    pType.value = pf.type;
    const pF1 = num(pf.f1, 1, 0);
    const pF2 = num(pf.f2, 1, 0);
    const pLoad = document.createElement('button');
    pLoad.type = 'button';
    pLoad.textContent = 'Cargar en los PSoC';
    const pFactory = document.createElement('button');
    pFactory.type = 'button';
    pFactory.textContent = 'FIR de fábrica';
    const pStatus = document.createElement('span');
    pStatus.className = 'filter-status';
    const refreshPsoc = () => {
      pF2.disabled = pType.value === 'lp' || pType.value === 'hp';
      pStatus.textContent = pf.active
        ? `FIR del PSoC: ${pf.type} ${pf.f1}${pf.type === 'lp' || pf.type === 'hp' ? '' : '-' + pf.f2} Hz`
        : 'FIR del PSoC: de fábrica';
    };
    pType.addEventListener('change', refreshPsoc);
    pLoad.addEventListener('click', () => {
      pf.type = pType.value;
      pf.f1 = parseFloat(pF1.value) || 0;
      pf.f2 = parseFloat(pF2.value) || 0;
      pf.active = true;
      saveGroupState(group, st);
      refreshPsoc();
      emitChange(group, 'firpsoc');
    });
    pFactory.addEventListener('click', () => {
      pf.active = false;
      saveGroupState(group, st);
      refreshPsoc();
      emitChange(group, 'firpsoc');
    });
    const psocRow = document.createElement('div');
    psocRow.className = 'row';
    psocRow.append(labeled('FIR del PSoC', pType), labeled('F1 Hz', pF1), labeled('F2 Hz', pF2), pLoad, pFactory);
    box.append(psocRow, pStatus);
    refreshPsoc();

    refreshFir();
    refreshByGroup[group] = () => {
      type.value = st.firType;
      f1.value = String(st.f1);
      f2.value = String(st.f2);
      taps.value = String(st.taps);
      firHw.checked = !!st.firHw;
      pType.value = st.psocFir.type;
      pF1.value = String(st.psocFir.f1);
      pF2.value = String(st.psocFir.f2);
      refreshFir();
      refreshPsoc();
    };
    container.append(box);
  }

  common.addEventListener('change', () => {
    sameFir = common.checked;
    saveCommonFir(sameFir);
    warning.hidden = sameFir;
    if (!sameFir) return;
    copyFirState(states.geo, states.hammer);
    saveGroupState('hammer', states.hammer);
    refreshByGroup.hammer();
    onChange('geo', states.geo, 'fir');
    onChange('geo', states.geo, 'firpsoc');
    onChange('geo', states.geo, 'firhw');
  });

  // Firmas de ruido: una sola tabla para todos
  const sig = document.createElement('fieldset');
  sig.className = 'panel';
  sig.innerHTML = '<legend>Firmas de ruido</legend>';
  const file = document.createElement('input');
  file.type = 'file'; file.accept = '.json,application/json';
  file.addEventListener('change', () => {
    const f = file.files?.[0];
    if (f) onSignatures(f);
    file.value = '';
  });
  const sigInfo = document.createElement('span');
  sigInfo.id = 'filter-signatures-info';
  sigInfo.className = 'filter-status';
  sig.append(labeled('Cargar firmas_<placa>.json', file), ' ', sigInfo);
  container.append(sig);

  return {
    state: (group) => states[group],
    sameFir: () => sameFir,
    setSignatureInfo: (text) => { sigInfo.textContent = text; },
    setFirStatus: (group, text) => { if (statusEls[group]) statusEls[group].textContent = text; },
  };
}
