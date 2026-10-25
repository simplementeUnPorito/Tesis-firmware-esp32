import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { decodeGeoZ, GEOZ_SAMPLES } from './geo_codec.js';

const here = dirname(fileURLToPath(import.meta.url));
const tesis = resolve(here, '../../../../../../..');
const lab = resolve(tesis, 'lab/compresion');
const temp = mkdtempSync(resolve(tmpdir(), 'geoz-js-'));
const encoder = resolve(temp, process.platform === 'win32' ? 'emit_geo_vectors.exe' : 'emit_geo_vectors');

function verify(name) {
  const original = readFileSync(resolve(lab, name));
  const encodedPath = resolve(temp, `${name}.geoz`);
  execFileSync(encoder, [resolve(lab, name), encodedPath], { stdio: 'inherit' });
  const encoded = readFileSync(encodedPath);
  const recordBytes = GEOZ_SAMPLES * 4 + GEOZ_SAMPLES;
  assert.equal(original.length % recordBytes, 0, `${name}: registro truncado`);
  let at = 0;
  let mismatches = 0;
  const batches = original.length / recordBytes;
  for (let batch = 0; batch < batches; batch++) {
    assert.ok(at < encoded.length, `${name}: faltan payloads`);
    const base = batch * recordBytes;
    for (let version = 0; version < 2; version++) {
      const len = encoded[at++];
      const decoded = decodeGeoZ(encoded.subarray(at, at + len));
      at += len;
      for (let i = 0; i < GEOZ_SAMPLES; i++) {
        if (decoded.values[i] !== original.readInt32LE(base + i * 4)) mismatches++;
        if (decoded.flags[i] !== original[base + GEOZ_SAMPLES * 4 + i]) mismatches++;
      }
    }
  }
  assert.equal(at, encoded.length, `${name}: payloads sobrantes`);
  assert.equal(mismatches, 0, `${name}: diferencias`);
  console.log(`${name}: ${batches} lotes v1+v2, 0 diferencias`);
}

try {
  execFileSync('gcc', ['-std=c99', '-O2', resolve(lab, 'emit_geo_vectors.c'), '-o', encoder], {
    stdio: 'inherit',
  });
  verify('reales.bin');
  verify('martillo.bin');
} finally {
  rmSync(temp, { recursive: true, force: true });
}
