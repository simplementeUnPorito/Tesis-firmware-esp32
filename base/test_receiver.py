#!/usr/bin/env python3
"""
test_receiver.py — Prueba headless del parser de la base station.

No requiere WiFi ni ESP32.  Construye paquetes UDP sintéticos y los
decodifica con parse_node_packet(), verificando integridad y rangos.

Escenarios:
  1. Paquete válido de un nodo — decodificación correcta
  2. Marcador incorrecto — rechazado
  3. CRC incorrecto — rechazado
  4. Payload truncado — rechazado
  5. n_samples=0 — caso borde (paquete vacío pero válido)
  6. n_samples=30 (máximo del firmware) — OK
  7. Paquete con samples de 24-bit negativos (sign extension)
  8. Secuencia de 100 paquetes consecutivos — sin drops en el acumulador
  9. Paquetes de 3 nodos distintos — acumulados correctamente
 10. Paquete con timestamp = 0 (borde extremo)
"""

import sys
import os
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from receiver import parse_node_packet, NodeAccumulator, NODE_PKT_MARKER

SAMPLE_BYTES = 10


# ── Builder de paquetes ───────────────────────────────────────────────────────

def _xor_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
    return crc


def build_packet(node_id: int, seq: int, flags: int, timestamp_us: int,
                 samples: list[dict], corrupt_crc: bool = False,
                 bad_marker: bool = False) -> bytes:
    """Build a synthetic node UDP packet."""
    n = len(samples)
    marker = 0xFF if bad_marker else NODE_PKT_MARKER
    hdr = bytes([
        marker, node_id,
        seq & 0xFF, (seq >> 8) & 0xFF,
        flags, n,
    ])
    ts = struct.pack('<Q', timestamp_us)
    payload = bytearray()
    for s in samples:
        raw     = s.get('raw_input', 0)
        analog  = s.get('post_analog', 0)
        digital = s.get('post_digital', 0)
        gain    = s.get('gain_byte', 0x43)  # cfg=3, buf_gain=8
        sflags  = s.get('flags', 0)
        # raw int16 LE
        payload += struct.pack('<h', raw)
        # analog 3 bytes LE
        payload += bytes([analog & 0xFF, (analog >> 8) & 0xFF, (analog >> 16) & 0xFF])
        # digital 3 bytes LE
        payload += bytes([digital & 0xFF, (digital >> 8) & 0xFF, (digital >> 16) & 0xFF])
        payload += bytes([gain, sflags])

    body = hdr + ts + bytes(payload)
    crc  = _xor_crc(body)
    if corrupt_crc:
        crc ^= 0xFF   # flip all bits
    return body + bytes([crc])


def make_sample(raw=100, analog=5000, digital=4800, gain=0x43, flags=0):
    return {'raw_input': raw, 'post_analog': analog, 'post_digital': digital,
            'gain_byte': gain, 'flags': flags}


# ── Escenarios ────────────────────────────────────────────────────────────────

class Result:
    def __init__(self, name):
        self.name = name
        self.ok = True
        self.errors = []

    def fail(self, msg):
        self.ok = False
        self.errors.append(msg)

    def __str__(self):
        s = "PASS ✓" if self.ok else "FAIL ✗"
        lines = [f"  [{s}] {self.name}"]
        for e in self.errors:
            lines.append(f"         → {e}")
        return "\n".join(lines)


def run_scenarios():
    results = []

    # ─────────────────────────────────────────────────────────────────────────
    # 1. Paquete válido — decodificación completa
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 1 — Paquete válido (happy path)")
    samples = [make_sample(raw=150, analog=12000, digital=11500)] * 30
    pkt = build_packet(1, 42, 0x01, 1713484800000000, samples)
    d = parse_node_packet(pkt)
    if d is None:
        r.fail("parse_node_packet devolvió None en paquete válido")
    else:
        if d['node_id'] != 1:     r.fail(f"node_id={d['node_id']} esperado 1")
        if d['seq'] != 42:        r.fail(f"seq={d['seq']} esperado 42")
        if len(d['samples']) != 30: r.fail(f"n_samples={len(d['samples'])}")
        if d['samples'][0]['raw_input'] != 150:
            r.fail(f"raw_input={d['samples'][0]['raw_input']} esperado 150")
        if d['samples'][0]['post_analog'] != 12000:
            r.fail(f"post_analog={d['samples'][0]['post_analog']} esperado 12000")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 2. Marcador incorrecto — rechazado
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 2 — Marcador 0xFF incorrecto → rechazado")
    pkt = build_packet(1, 0, 0, 0, [make_sample()] * 5, bad_marker=True)
    if parse_node_packet(pkt) is not None:
        r.fail("No rechazó marcador inválido")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 3. CRC incorrecto — rechazado
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 3 — CRC corrompido → rechazado")
    pkt = build_packet(2, 1, 0, 100, [make_sample()] * 10, corrupt_crc=True)
    if parse_node_packet(pkt) is not None:
        r.fail("No rechazó CRC inválido")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 4. Payload truncado — rechazado
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 4 — Payload truncado a la mitad → rechazado")
    pkt = build_packet(1, 5, 0, 0, [make_sample()] * 30)
    truncated = pkt[:len(pkt) // 2]   # cortar a la mitad
    if parse_node_packet(truncated) is not None:
        r.fail("No rechazó paquete truncado")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 5. n_samples = 0 — caso borde (header OK, sin muestras)
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 5 — n_samples=0 (paquete vacío pero bien formado)")
    pkt = build_packet(1, 10, 0, 999, [])
    d = parse_node_packet(pkt)
    if d is None:
        r.fail("Rechazó paquete vacío válido")
    elif len(d['samples']) != 0:
        r.fail(f"n_samples={len(d['samples'])} esperado 0")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 6. n_samples = 30 — límite del firmware
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 6 — n_samples=30 (máximo del firmware) → OK")
    pkt = build_packet(3, 7, 0x07, 42000000, [make_sample()] * 30)
    d = parse_node_packet(pkt)
    if d is None:
        r.fail("Rechazó 30 muestras")
    elif len(d['samples']) != 30:
        r.fail(f"n_samples={len(d['samples'])} esperado 30")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 7. Sign extension de valores negativos 24-bit
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 7 — Valores negativos 24-bit (sign extension)")
    # Valor -1 en 24-bit = 0xFFFFFF
    neg_sample = make_sample(raw=-2048, analog=-8388608, digital=-1)
    pkt = build_packet(1, 0, 0, 0, [neg_sample] * 5)
    d = parse_node_packet(pkt)
    if d is None:
        r.fail("Rechazó paquete con valores negativos")
    else:
        s = d['samples'][0]
        if s['raw_input'] != -2048:
            r.fail(f"raw_input={s['raw_input']} esperado -2048")
        if s['post_analog'] != -8388608:
            r.fail(f"post_analog={s['post_analog']} esperado -8388608")
        if s['post_digital'] != -1:
            r.fail(f"post_digital={s['post_digital']} esperado -1")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 8. Secuencia de 100 paquetes — acumulador sin drops
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 8 — 100 paquetes consecutivos → acumulador sin drops")
    acc = NodeAccumulator(1)
    for seq in range(100):
        pkt = build_packet(1, seq, 0, seq * 20000, [make_sample()] * 30)
        d = parse_node_packet(pkt)
        if d is None:
            r.fail(f"Paquete seq={seq} rechazado")
            break
        acc.add_batch(d)
    if acc.drops != 0:
        r.fail(f"Acumulador reportó {acc.drops} drops en secuencia continua")
    if len(acc.digital_sig) != 100 * 30:
        r.fail(f"Muestras acumuladas={len(acc.digital_sig)} esperado {100*30}")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 9. 3 nodos distintos — cada acumulador independiente
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 9 — 3 nodos distintos (IDs 1, 2, 3)")
    accumulators = {nid: NodeAccumulator(nid) for nid in [1, 2, 3]}
    for seq in range(20):
        for nid in [1, 2, 3]:
            pkt = build_packet(nid, seq, 0, seq * 20000,
                               [make_sample(raw=nid * 100)] * 30)
            d = parse_node_packet(pkt)
            if d is None:
                r.fail(f"Nodo {nid} seq {seq} rechazado")
                break
            accumulators[nid].add_batch(d)
    for nid in [1, 2, 3]:
        acc = accumulators[nid]
        expected = 20 * 30
        if len(acc.digital_sig) != expected:
            r.fail(f"Nodo {nid}: {len(acc.digital_sig)} muestras (esperado {expected})")
        # Verificar que los raw values son correctos por nodo
        arr = acc.to_arrays()
        if arr['raw'][0] != nid * 100:
            r.fail(f"Nodo {nid}: raw[0]={arr['raw'][0]} esperado {nid*100}")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 10. Timestamp = 0 (Unix epoch borde extremo)
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 10 — Timestamp=0 (Unix epoch borde)")
    pkt = build_packet(1, 0, 0, 0, [make_sample()] * 10)
    d = parse_node_packet(pkt)
    if d is None:
        r.fail("Rechazó timestamp=0")
    elif d['timestamp_us'] != 0:
        r.fail(f"timestamp_us={d['timestamp_us']} esperado 0")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 11. Drop detection: secuencia con salto (paquete perdido en el medio)
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 11 — Drop detection: salto de 3 en secuencia")
    acc = NodeAccumulator(1)
    for seq in [0, 1, 2, 6, 7, 8]:   # seq 3,4,5 perdidos
        pkt = build_packet(1, seq, 0, seq * 20000, [make_sample()] * 10)
        d = parse_node_packet(pkt)
        if d:
            acc.add_batch(d)
    if acc.drops != 3:
        r.fail(f"drops={acc.drops} esperado 3 (seq 3,4,5 perdidos)")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 12. Paquete de 1 sola muestra
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 12 — n_samples=1 (mínimo no-vacío)")
    pkt = build_packet(1, 0, 0, 0, [make_sample(raw=999)])
    d = parse_node_packet(pkt)
    if d is None:
        r.fail("Rechazó paquete con 1 muestra")
    elif d['samples'][0]['raw_input'] != 999:
        r.fail(f"raw_input={d['samples'][0]['raw_input']} esperado 999")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 13. node_id extremos: 0 y 255
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 13 — node_id=0 y node_id=255 son aceptados")
    for nid in [0, 255]:
        pkt = build_packet(nid, 1, 0, 0, [make_sample()] * 5)
        d = parse_node_packet(pkt)
        if d is None:
            r.fail(f"Rechazó node_id={nid}")
        elif d['node_id'] != nid:
            r.fail(f"node_id={d['node_id']} esperado {nid}")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 14. Seq rollover 65535→0 — sin falso positivo de drop
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 14 — Seq rollover 65535→0 → drops=0 (sin falso positivo)")
    acc = NodeAccumulator(1)
    for seq in [65534, 65535, 0, 1]:   # rollover correcto
        pkt = build_packet(1, seq, 0, seq * 1000, [make_sample()] * 5)
        d = parse_node_packet(pkt)
        if d:
            acc.add_batch(d)
    if acc.drops != 0:
        r.fail(f"drops={acc.drops} esperado 0 (rollover legítimo)")
    results.append(r)

    # ─────────────────────────────────────────────────────────────────────────
    # 15. Timestamp máximo uint64 (2^64-1) — sin crash
    # ─────────────────────────────────────────────────────────────────────────
    r = Result("Escenario 15 — timestamp_us = 2^64-1 (max uint64) → sin crash")
    MAX_TS = (1 << 64) - 1
    try:
        pkt = build_packet(1, 0, 0, MAX_TS, [make_sample()] * 3)
        d = parse_node_packet(pkt)
        if d is None:
            r.fail("Rechazó paquete con timestamp máximo")
        elif d['timestamp_us'] != MAX_TS:
            r.fail(f"timestamp_us={d['timestamp_us']} esperado {MAX_TS}")
    except Exception as e:
        r.fail(f"Excepción: {e}")
    results.append(r)

    return results


def main():
    print("=" * 65)
    print("  TEST RECEIVER BASE STATION (headless)")
    print("=" * 65)

    results = run_scenarios()
    passed  = sum(1 for r in results if r.ok)
    failed  = len(results) - passed

    for r in results:
        print(r)

    print("─" * 65)
    print(f"  Total: {len(results)}  |  PASS: {passed}  |  FAIL: {failed}")
    print("=" * 65)
    return failed == 0


if __name__ == '__main__':
    ok = main()
    sys.exit(0 if ok else 1)
