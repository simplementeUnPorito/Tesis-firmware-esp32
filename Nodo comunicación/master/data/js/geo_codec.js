// Decoder sin perdida compatible con geo_codec.h (formatos v1 y v2).

export const GEOZ_SAMPLES = 30;
const ORDER_MASK = 0x03;
const ESCAPE = 0x04;
const HAS_FLAGS = 0x08;
const FORMAT_V2 = 0x10;

function signed24(bytes, pos) {
  const u = bytes[pos] | (bytes[pos + 1] << 8) | (bytes[pos + 2] << 16);
  return (u & 0x800000) ? u - 0x1000000 : u;
}

function predictor(values, i, order) {
  if (order === 0) return 0;
  if (order === 1) return values[i - 1];
  if (order === 2) return 2 * values[i - 1] - values[i - 2];
  return 3 * values[i - 1] - 3 * values[i - 2] + values[i - 3];
}

function unzigzag(value) {
  return (value % 2) ? -Math.floor(value / 2) - 1 : value / 2;
}

class BitReader {
  constructor(bytes, pos) {
    this.bytes = bytes;
    this.start = pos;
    this.bit = 0;
  }

  read() {
    if (this.bit >= (this.bytes.length - this.start) * 8) throw new Error('GeoZ truncado');
    const value = (this.bytes[this.start + (this.bit >> 3)] >> (7 - (this.bit & 7))) & 1;
    this.bit++;
    return value;
  }

  endPos() {
    return this.start + Math.ceil(this.bit / 8);
  }
}

function decodeFlags(bytes, pos, mode) {
  const flags = new Uint8Array(GEOZ_SAMPLES);
  if (mode & HAS_FLAGS) {
    let at = 0;
    while (at < GEOZ_SAMPLES) {
      if (pos + 2 > bytes.length) throw new Error('RLE de marcas truncado');
      const value = bytes[pos++];
      const run = bytes[pos++];
      if (run === 0 || at + run > GEOZ_SAMPLES) throw new Error('RLE de marcas invalido');
      flags.fill(value, at, at + run);
      at += run;
    }
  }
  if (pos !== bytes.length) throw new Error('Bytes sobrantes en GeoZ');
  return flags;
}

function decodeV1(bytes, values, mode) {
  let pos;
  if (mode & ESCAPE) {
    if (bytes.length < 91) throw new Error('GeoZ v1 crudo truncado');
    for (let i = 0; i < GEOZ_SAMPLES; i++) values[i] = signed24(bytes, 1 + 3 * i);
    pos = 91;
  } else {
    if ((mode & ORDER_MASK) !== 1 || bytes.length < 5) throw new Error('GeoZ v1 invalido');
    const k = bytes[4];
    if (k > 23) throw new Error('Rice k invalido');
    values[0] = signed24(bytes, 1);
    const reader = new BitReader(bytes, 5);
    for (let i = 1; i < GEOZ_SAMPLES; i++) {
      let q = 0;
      while (reader.read() === 1) {
        if (++q > (1 << 27)) throw new Error('Cociente Rice invalido');
      }
      let remainder = 0;
      for (let j = 0; j < k; j++) remainder = remainder * 2 + reader.read();
      const value = values[i - 1] + unzigzag(q * (2 ** k) + remainder);
      const u24 = ((value % 0x1000000) + 0x1000000) % 0x1000000;
      values[i] = u24 >= 0x800000 ? u24 - 0x1000000 : u24;
    }
    pos = reader.endPos();
  }
  return decodeFlags(bytes, pos, mode);
}

export function decodeGeoZ(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  if (bytes.length < 1) throw new Error('GeoZ vacio');
  const mode = bytes[0];
  const values = new Int32Array(GEOZ_SAMPLES);
  if (!(mode & FORMAT_V2)) {
    return { values, flags: decodeV1(bytes, values, mode) };
  }
  if (mode & 0xE0) throw new Error('Bits reservados de GeoZ');

  const order = mode & ORDER_MASK;
  let pos;
  if (mode & ESCAPE) {
    if (bytes.length < 91) throw new Error('GeoZ v2 crudo truncado');
    for (let i = 0; i < GEOZ_SAMPLES; i++) values[i] = signed24(bytes, 1 + 3 * i);
    pos = 91;
  } else {
    if (bytes.length < 2 + 3 * order) throw new Error('GeoZ v2 truncado');
    const k = bytes[1];
    if (k > 31) throw new Error('Rice k invalido');
    for (let i = 0; i < order; i++) values[i] = signed24(bytes, 2 + 3 * i);
    const reader = new BitReader(bytes, 2 + 3 * order);
    for (let i = order; i < GEOZ_SAMPLES; i++) {
      let q = 0;
      while (reader.read() === 0) {
        if (++q > (1 << 27)) throw new Error('Cociente Rice invalido');
      }
      let remainder = 0;
      for (let j = 0; j < k; j++) remainder = remainder * 2 + reader.read();
      const value = predictor(values, i, order) + unzigzag(q * (2 ** k) + remainder);
      if (value < -8388608 || value > 8388607) throw new Error('Muestra GeoZ fuera de int24');
      values[i] = value;
    }
    pos = reader.endPos();
  }
  return { values, flags: decodeFlags(bytes, pos, mode) };
}
