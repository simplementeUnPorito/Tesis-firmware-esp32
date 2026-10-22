"""Prueba del maestro por USB (921600, paquetes [0x56][nodo][tipo][b2][b1][b0]).

Uso:
    python probar_maestro.py COM6 hello
    python probar_maestro.py COM6 captura 4          # ARM + START de N lotes
    python probar_maestro.py COM6 vdac 1 128         # VDAC global de un nodo SERIAL

'hello' lista lo que el maestro sabe de cada nodo: psoc, fs, GEO/HAMMER,
SD y enlace SERIAL/I2C. Si COM6 se cae a mitad de la prueba, sospechar de la
alimentación del maestro antes que del firmware.
"""
import sys
import threading
import time

import serial

HW = {0: 'GEO', 1: 'HAMMER', 0xFF: 'UNKNOWN'}
LINK = {0: 'SERIAL', 1: 'I2C', 0xFF: 'UNKNOWN'}


class Maestro:
    def __init__(self, port):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.write_timeout = port, 921600, 0.05, 2
        s.dtr = False
        s.rts = False
        s.open()
        self.s = s
        self.pkts = []
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self._rx, daemon=True)
        self.thread.start()

    def _rx(self):
        buf = bytearray()
        while self.running:
            try:
                buf += self.s.read(8192)
            except serial.SerialException as e:
                print('puerto perdido:', e)
                self.running = False
                return
            i = 0
            while len(buf) - i >= 6:
                if buf[i] == 0x56:
                    with self.lock:
                        self.pkts.append(bytes(buf[i:i + 6]))
                    i += 6
                else:
                    i += 1
            del buf[:i]

    def take(self):
        with self.lock:
            r, self.pkts = self.pkts, []
        return r

    def std(self, cmd, p=0):
        self.s.write(bytes([0xAB, cmd, p, cmd ^ p]))

    def std16(self, cmd, n):
        lo, hi = n & 0xFF, n >> 8
        self.s.write(bytes([0xAB, cmd, lo, hi, cmd ^ lo ^ hi]))

    def directed7(self, node, sub, p1, p2):
        self.s.write(bytes([0xAB, 0xBD, node, sub, p1, p2, node ^ sub ^ p1 ^ p2]))

    def wait(self, pred, timeout):
        got = []
        t0 = time.time()
        while time.time() - t0 < timeout and self.running:
            for p in self.take():
                got.append(p)
                if pred(p):
                    return got, p
            time.sleep(0.02)
        return got, None

    def close(self):
        self.running = False
        self.thread.join(1)
        self.s.close()


def hello(m):
    m.std(0xA5)
    got, _ = m.wait(lambda p: False, 4)
    nodes = {}
    for p in got:
        if p[2] != 0xFD or p[1] == 0xFF:
            continue
        n = nodes.setdefault(p[1], {})
        if p[3] == 0x01:
            n['psoc'] = p[4]
        elif p[3] == 0x05:
            n['fs'] = (p[4] << 8) | p[5]
        elif p[3] == 0x06:
            n['hw'] = HW.get(p[4], p[4])
        elif p[3] == 0x07:
            n['sd'] = p[4]
        elif p[3] == 0x08:
            n['enlace'] = LINK.get(p[4], p[4])
    if not nodes:
        print('sin HELLO de ningún nodo')
    for node, info in sorted(nodes.items()):
        info.setdefault('enlace', 'LEGACY (sin link_model)')
        print(f'nodo {node}: {info}')


def captura(m, n):
    m.std16(0xAE, n)
    m.std(0xA2, 0)
    _, ready = m.wait(lambda p: p[2] == 0xFE and p[3] >= 1, 10)
    print('READY:', ready.hex(' ') if ready else 'no llegó (¿ningún nodo armado?)')
    if not ready:
        return
    m.std16(0xA3, n)
    got, _ = m.wait(lambda p: False, 20 + n)
    per_node = {}
    for p in got:
        if p[2] == 0x00 and p[1] != 0:
            per_node.setdefault(p[1], []).append(int.from_bytes(p[3:6], 'big', signed=True))
    for node, vals in sorted(per_node.items()):
        print(f'nodo {node}: {len(vals)} muestras, min {min(vals)}, max {max(vals)}')
    if not per_node:
        print('sin muestras')
    acks = [p.hex(' ') for p in got if p[2] == 0x07]
    print('ACKs:', acks[:20])


def vdac(m, node, code):
    m.directed7(node, 0xAA, 0x40, code)
    _, ack = m.wait(lambda p: p[2] == 0x07 and p[1] == node and p[3] == 0xAA, 5)
    print('ACK VDAC:', ack.hex(' ') if ack else 'sin ACK')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    m = Maestro(sys.argv[1])
    try:
        time.sleep(0.3)
        m.take()
        action = sys.argv[2]
        if action == 'hello':
            hello(m)
        elif action == 'captura':
            captura(m, int(sys.argv[3]) if len(sys.argv) > 3 else 4)
        elif action == 'vdac':
            vdac(m, int(sys.argv[3]), int(sys.argv[4]))
        else:
            print(__doc__)
    finally:
        m.close()


if __name__ == '__main__':
    main()
