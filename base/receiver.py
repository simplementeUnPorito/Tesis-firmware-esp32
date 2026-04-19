#!/usr/bin/env python3
"""
receiver.py — Base station UDP receiver for multi-node geophone network.

Receives timestamped data batches from ESP32 nodes, validates CRC,
decodes samples, and stores them for spatio-temporal analysis.

Protocol:
    [0xBC][node_id][seq_lo][seq_hi][flags][n][ts×8][samples...][crc]
    Each sample: [raw×2][analog×3][digital×3][gain][flags] = 10 bytes

Usage:
    python receiver.py [--port 5005] [--output data.npz]
"""

import argparse
import socket
import struct
import time
import sys
import signal
import threading
import collections
import numpy as np

UDP_PORT       = 5005
NODE_PKT_MARKER = 0xBC
SAMPLE_BYTES   = 10
MAX_NODES      = 8

# ── Packet parser ─────────────────────────────────────────────────────────────

def _xor_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
    return crc


def _sign24(b0, b1, b2) -> int:
    val = b0 | (b1 << 8) | (b2 << 16)
    return val - 0x1000000 if (val & 0x800000) else val


def parse_node_packet(raw: bytes) -> dict | None:
    """Decode one UDP packet.  Returns None on bad marker or CRC mismatch."""
    if len(raw) < 15 or raw[0] != NODE_PKT_MARKER:  # min: 6 hdr + 8 ts + 1 crc
        return None
    if _xor_crc(raw[:-1]) != raw[-1]:
        return None

    node_id      = raw[1]
    seq          = raw[2] | (raw[3] << 8)
    global_flags = raw[4]
    n_samples    = raw[5]
    timestamp_us = struct.unpack_from('<Q', raw, 6)[0]

    expected = 14 + n_samples * SAMPLE_BYTES + 1
    if len(raw) < expected:
        return None

    samples = []
    offset  = 14
    for _ in range(n_samples):
        p = raw[offset:offset + SAMPLE_BYTES]
        raw_input    = struct.unpack_from('<h', p, 0)[0]
        post_analog  = _sign24(p[2], p[3], p[4])
        post_digital = _sign24(p[5], p[6], p[7])
        gain_byte    = p[8]
        s_flags      = p[9]
        samples.append({
            'raw_input':    raw_input,
            'post_analog':  post_analog,
            'post_digital': post_digital,
            'gain_byte':    gain_byte,
            'flags':        s_flags,
        })
        offset += SAMPLE_BYTES

    return {
        'node_id':      node_id,
        'seq':          seq,
        'flags':        global_flags,
        'timestamp_us': timestamp_us,
        'samples':      samples,
    }


# ── Accumulator: stores data per node for offline analysis ────────────────────

class NodeAccumulator:
    def __init__(self, node_id: int):
        self.node_id     = node_id
        self.timestamps  = []   # us since epoch, one per batch
        self.digital_sig = []   # post_digital values, flat list
        self.analog_sig  = []
        self.raw_sig     = []
        self.seq_last    = None
        self.drops       = 0
        self.pkts        = 0

    def add_batch(self, pkt: dict):
        self.pkts += 1
        # Detect dropped batches
        if self.seq_last is not None:
            gap = (pkt['seq'] - self.seq_last) & 0xFFFF
            if gap > 1:
                self.drops += gap - 1
        self.seq_last = pkt['seq']
        self.timestamps.append(pkt['timestamp_us'])
        for s in pkt['samples']:
            self.digital_sig.append(s['post_digital'])
            self.analog_sig.append(s['post_analog'])
            self.raw_sig.append(s['raw_input'])

    def to_arrays(self) -> dict:
        return {
            'node_id':    self.node_id,
            'timestamps': np.array(self.timestamps, dtype=np.uint64),
            'digital':    np.array(self.digital_sig, dtype=np.int32),
            'analog':     np.array(self.analog_sig, dtype=np.int32),
            'raw':        np.array(self.raw_sig, dtype=np.int16),
            'drops':      self.drops,
            'pkts':       self.pkts,
        }


# ── Main receiver ─────────────────────────────────────────────────────────────

class BaseReceiver:
    def __init__(self, port: int = UDP_PORT):
        self.port        = port
        self._nodes: dict[int, NodeAccumulator] = {}
        self._running    = False
        self._pkt_good   = 0
        self._pkt_bad    = 0
        self._lock       = threading.Lock()

    def start(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(('', self.port))
        self._sock.settimeout(1.0)
        self._running = True
        print(f"[BASE] Escuchando en UDP :{self.port}", flush=True)

    def stop(self):
        self._running = False

    def run(self):
        """Blocking receive loop — run in main thread or a thread."""
        last_report = time.monotonic()
        while self._running:
            try:
                data, addr = self._sock.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                break

            pkt = parse_node_packet(data)
            if pkt:
                with self._lock:
                    nid = pkt['node_id']
                    if nid not in self._nodes:
                        self._nodes[nid] = NodeAccumulator(nid)
                        print(f"[BASE] Nuevo nodo detectado: {nid} desde {addr[0]}", flush=True)
                    self._nodes[nid].add_batch(pkt)
                self._pkt_good += 1
            else:
                self._pkt_bad += 1

            # Status report each 10 s
            now = time.monotonic()
            if now - last_report >= 10.0:
                last_report = now
                self._print_status()

    def save(self, path: str):
        """Save accumulated data from all nodes to .npz file."""
        arrays = {}
        with self._lock:
            for nid, acc in self._nodes.items():
                d = acc.to_arrays()
                for key, val in d.items():
                    if isinstance(val, np.ndarray):
                        arrays[f"node{nid}_{key}"] = val
                    else:
                        arrays[f"node{nid}_{key}"] = np.array([val])
        np.savez_compressed(path, **arrays)
        print(f"[BASE] Datos guardados en {path}  ({len(arrays)} arrays)", flush=True)

    def _print_status(self):
        with self._lock:
            print(f"[BASE] pkts_ok={self._pkt_good}  bad={self._pkt_bad}  "
                  f"nodos={list(self._nodes.keys())}", flush=True)
            for nid, acc in self._nodes.items():
                n_samples = len(acc.digital_sig)
                print(f"  Nodo {nid}: {acc.pkts} batches  "
                      f"{n_samples} muestras  drops={acc.drops}", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Base station UDP receiver")
    parser.add_argument('--port',   type=int, default=UDP_PORT,       help="Puerto UDP")
    parser.add_argument('--output', type=str, default='geophone_data.npz', help="Archivo de salida")
    parser.add_argument('--time',   type=float, default=0, help="Segundos de captura (0=infinito)")
    args = parser.parse_args()

    rx = BaseReceiver(args.port)
    rx.start()

    def on_sigint(sig, frame):
        print("\n[BASE] Deteniendo...", flush=True)
        rx.stop()

    signal.signal(signal.SIGINT, on_sigint)

    t0 = time.monotonic()
    rx_thread = threading.Thread(target=rx.run, daemon=True)
    rx_thread.start()

    while rx._running:
        time.sleep(0.5)
        if args.time > 0 and (time.monotonic() - t0) >= args.time:
            rx.stop()

    rx_thread.join(timeout=3)
    rx.save(args.output)


if __name__ == '__main__':
    main()
