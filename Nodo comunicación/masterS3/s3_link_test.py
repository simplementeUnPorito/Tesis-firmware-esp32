"""Prueba de dos S3 por USB + ESP-NOW, sin PSoC. Genera una rampa sintética.

python s3_link_test.py --master COM4 --slave COM8 --output artifacts/link.json
No graba firmware. Activa debug de laboratorio y lo apaga incluso ante errores.
"""
import argparse
from collections import Counter
import json
import re
from pathlib import Path
import time
import serial


def open_port(port, baud):
    dev = serial.Serial(port=None, baudrate=baud, timeout=0.02)
    dev.port, dev.dtr, dev.rts = port, False, False
    dev.open()
    return dev


def command(dev, cmd, param=0):
    data = [cmd, param & 255]
    if cmd in (0xAE, 0xA3, 0xAD):
        data.append(param >> 8)
    crc = 0
    for b in data:
        crc ^= b
    dev.write(bytes([0xAB] + data + [crc]))


def debug_node(dev, enable):
    # Directed command: other nearby nodes must not enter debug mode.
    dev.write(bytes([0xAB, 0xBD, 1, 0xA7, int(enable), 1 ^ 0xA7 ^ int(enable)]))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--master', required=True)
    ap.add_argument('--slave', required=True)
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--output', type=Path, default=Path('artifacts/link.json'))
    args = ap.parse_args()
    packets, buffer, slave_log = [], bytearray(), bytearray()
    with open_port(args.master, 921600) as master, open_port(args.slave, 115200) as slave:
        def collect(seconds):
            end = time.monotonic() + seconds
            start = len(packets)
            while time.monotonic() < end:
                buffer.extend(master.read(master.in_waiting or 1))
                slave_log.extend(slave.read(slave.in_waiting))
                while len(buffer) >= 6:
                    if buffer[0] != 0x56:
                        del buffer[0]
                        continue
                    packets.append(bytes(buffer[:6]))
                    del buffer[:6]
            return packets[start:]
        try:
            collect(2)
            debug_node(master, False)
            collect(2)
            command(master, 0xAE, 0)
            collect(1)
            command(master, 0xA1, 1)
            # Recien arrancado, el esclavo puede perder el primer dirigido: reintentar hasta el ACK.
            run = []
            for _ in range(5):
                debug_node(master, True)
                run += collect(1)
                if any(p[1:5] == bytes([1, 7, 0xA7, 1]) for p in run):
                    break
            run += collect(args.seconds)
            debug_node(master, False)
            collect(1)  # drain queued packets before checking STOP
            stopped = collect(2)
            slave.write(b'status\n')
            collect(1)
        finally:
            debug_node(master, False)
            command(master, 0xA1, 0)
            collect(1)
    samples = [int.from_bytes(p[3:], 'big', signed=True) for p in run if p[1:3] == b'\x01\x00']
    discontinuities = sum(b != a + 1 for a, b in zip(samples, samples[1:]))
    checks = {
        'slave_debug_ack': any(p[1:5] == bytes([1, 7, 0xA7, 1]) for p in run),
        'slave_radio_data': len(samples) > args.seconds * 1000,
        'ramp_contiguous': len(samples) > 0 and discontinuities == 0,
        'debug_stops': not any(p[1:3] == b'\x01\x00' for p in stopped),
        'slave_console': b'USB_STATUS' in slave_log,
        'slave_stopped': bool(re.search(rb'USB_STATUS[^\n]*state=0\b', slave_log)),
        'no_crash': not any(x in slave_log for x in (b'Guru Meditation', b'Backtrace:', b'assert failed')),
    }
    result = {'checks': checks, 'passed': all(checks.values()), 'seconds': args.seconds,
              'slave_samples': len(samples), 'discontinuities': discontinuities,
              'first_samples': samples[:10], 'last_samples': samples[-10:],
              'packet_counts': dict(Counter(f'{p[1]:02x}:{p[2]:02x}' for p in packets)),
              'slave_log': slave_log.decode(errors='replace')}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k != 'slave_log'}, indent=2))
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
