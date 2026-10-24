// Pestaña Filtros: una sola configuracion de procesamiento por grupo (todos los
// GEO comparten la suya; el HAMMER tiene otra). Reemplaza los controles de
// filtro que antes tenia cada esclavo.

const STORAGE_PREFIX = 'geophone_scope_web.filter_group_';
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
  };
}

export function loadGroupState(group) {
  const base = defaultGroupState();
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_PREFIX + group) || 'null');
    if (saved && typeof saved === 'object') {
      return { ...base, ...saved, clean: { ...base.clean, ...(saved.clean || {}) } };
    }
  } catch (_) { /* sin almacenamiento: valores por defecto */ }
  return base;
}

function saveGroupState(group, state) {
  try { localStorage.setItem(STORAGE_PREFIX + group, JSON.stringify(state)); } catch (_) {}
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
  const states = {};
  const statusEls = {};
  for (const [group, title] of FILTER_GROUPS) {
    const st = loadGroupState(group);
    states[group] = st;
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
      onChange(group, st, 'fir');
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
      onChange(group, st, 'firhw');
    });
    const hwRow = document.createElement('div');
    hwRow.className = 'row';
    hwRow.append(labeled('FIR hardware del PSoC', firHw));
    box.append(hwRow);

    refreshFir();
    container.append(box);
  }

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
    setSignatureInfo: (text) => { sigInfo.textContent = text; },
    setFirStatus: (group, text) => { if (statusEls[group]) statusEls[group].textContent = text; },
  };
}
