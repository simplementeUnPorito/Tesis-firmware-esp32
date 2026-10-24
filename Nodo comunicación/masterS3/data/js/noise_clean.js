// Limpieza de interferencias sincronizadas por las marcas [indice, flags].

export const NOISE_BITS = Object.freeze({
  SD_IO: 0x01,
  SD_SYNC: 0x02,
  RADIO_TX: 0x04,
  I2C: 0x08,
  AP_BEACON: 0x10,
  MASTER_RX: 0x20,
});

export function removeMedianOffset(values) {
  const source = ArrayBuffer.isView(values) ? values : new Float64Array(values || []);
  if (!source.length) return new Float64Array(0);
  const out = new Float64Array(source);
  const select = (k) => {
    let left = 0;
    let right = out.length - 1;
    while (left < right) {
      const pivot = out[(left + right) >> 1];
      let i = left;
      let j = right;
      while (i <= j) {
        while (out[i] < pivot) i++;
        while (out[j] > pivot) j--;
        if (i <= j) {
          const value = out[i]; out[i] = out[j]; out[j] = value;
          i++; j--;
        }
      }
      if (k <= j) right = j;
      else if (k >= i) left = i;
      else return out[k];
    }
    return out[k];
  };
  const middle = out.length >> 1;
  const upper = select(middle);
  const median = out.length & 1 ? upper : (upper + select(middle - 1)) / 2;
  // quickselect reordena la copia: restaurar el orden desde la fuente al restar.
  for (let i = 0; i < source.length; i++) out[i] = source[i] - median;
  return out;
}

export function flagsFromMarks(marks, length) {
  const flags = new Uint8Array(length);
  for (const mark of marks || []) {
    const index = Number(mark?.[0]);
    if (Number.isInteger(index) && index >= 0 && index < length) flags[index] |= Number(mark[1]) & 0xFF;
  }
  return flags;
}

function runs(flags, bit) {
  const result = [];
  let start = -1;
  for (let i = 0; i <= flags.length; i++) {
    const active = i < flags.length && (flags[i] & bit) !== 0;
    if (active && start < 0) start = i;
    if (!active && start >= 0) {
      result.push([start, i]);
      start = -1;
    }
  }
  return result;
}

function mergedWindows(flags, bit, margin) {
  const result = [];
  for (const [start, end] of runs(flags, bit)) {
    const lo = Math.max(0, start - margin);
    const hi = Math.min(flags.length, end + margin);
    const previous = result[result.length - 1];
    if (previous && lo <= previous[1]) previous[1] = Math.max(previous[1], hi);
    else result.push([lo, hi]);
  }
  return result;
}

function interpolateMarked(values, flags, bit, margin, cubic) {
  const out = new Float64Array(values);
  for (const [lo, hi] of mergedWindows(flags, bit, margin)) {
    if (lo < 1 || hi >= out.length) continue;
    const left = out[lo - 1];
    const right = out[hi];
    const count = hi - lo;
    for (let j = 0; j < count; j++) {
      const t = (j + 1) / (count + 1);
      if (cubic && lo >= 2 && hi + 1 < out.length) {
        const p0 = out[lo - 2];
        const p1 = left;
        const p2 = right;
        const p3 = out[hi + 1];
        const t2 = t * t;
        const t3 = t2 * t;
        out[lo + j] = 0.5 * ((2 * p1) + (-p0 + p2) * t +
          (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
      } else {
        out[lo + j] = left + (right - left) * t;
      }
    }
  }
  return out;
}

function templateValues(spec, which, voltsPerCount) {
  const direct = spec?.[`${which}_template`];
  const nested = spec?.[which]?.template;
  const compact = spec?.[which];
  const source = Array.isArray(direct) ? direct
    : (Array.isArray(compact) ? compact : (Array.isArray(nested) ? nested : []));
  return Float64Array.from(source, (value) => {
    const numeric = Number(value);
    return (Number.isFinite(numeric) ? numeric : 0) * voltsPerCount;
  });
}

function subtractOne(out, center, template, pre) {
  if (!template.length) return;
  const lo = center - pre;
  if (lo < 0 || lo + template.length > out.length) return;
  // X@c ya contiene la amplitud estimada conjunta: no reajustar por evento.
  for (let i = 0; i < template.length; i++) out[lo + i] -= template[i];
}

function subtractTemplates(values, flags, bit, spec, voltsPerCount, params) {
  const out = new Float64Array(values);
  const preValue = Number(spec?.pre);
  const pre = Math.max(0, Number.isFinite(preValue) ? preValue : 20);
  const onset = templateValues(spec, 'onset', voltsPerCount);
  const offset = templateValues(spec, 'offset', voltsPerCount);
  for (const [start, end] of runs(flags, bit)) {
    subtractOne(out, start, onset, pre);
    if (params?.offset !== false && offset.length) subtractOne(out, end, offset, pre);
  }
  return out;
}

function recommendation(spec) {
  const rec = spec?.recommended_method;
  if (typeof rec === 'string') return { method: rec, parameters: {} };
  if (rec && typeof rec === 'object') return { method: rec.method || 'interp', parameters: rec.parameters || {} };
  return { method: spec?.method || 'interp', parameters: spec || {} };
}

export function cleanMarkedNoise(values, flags, bit, signatures, typeName, countsPerVolt) {
  let marked = false;
  for (let i = 0; i < flags.length; i++) {
    if (flags[i] & bit) { marked = true; break; }
  }
  if (!marked) return values;
  const spec = signatures?.types?.[typeName] || {};
  const rec = recommendation(spec);
  const params = rec.parameters || {};
  const voltsPerCount = countsPerVolt > 0 ? 1 / countsPerVolt : 1;
  if (rec.method === 'none') return values;
  if ((rec.method === 'template' || rec.method === 'template_xcorr') &&
      (Array.isArray(spec.onset) || Array.isArray(spec.onset_template) || Array.isArray(spec?.onset?.template))) {
    return subtractTemplates(values, flags, bit, spec, voltsPerCount, params);
  }
  const margin = Math.max(0, Number(params.margin ?? spec.margin ?? 2) || 0);
  const cubic = rec.method === 'cubic' || params.kind === 'cubic';
  return interpolateMarked(values, flags, bit, margin, cubic);
}

export function signatureLabel(signatures) {
  const placa = signatures?.placa || 'sin placa';
  const fecha = signatures?.fecha || 'sin fecha';
  return `${placa} · ${fecha}`;
}
