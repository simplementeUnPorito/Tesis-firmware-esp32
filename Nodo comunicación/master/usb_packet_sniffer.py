"""Read the ESP32 master USB transport as binary 0x56 packets.

COM8 is the MATLAB/Python binary transport, not a text log. Opening the CP210x
port can reset the ESP32; this tool keeps DTR/RTS low and never writes unless
--status-probe is passed.
"""

from __future__ import annotations

import argparse
import collections
import sys
import time

try:
    import serial
except ImportError as exc:  # pragma: no cover - depends on local env
    raise SystemExit("pyserial is required: pip install pyserial") from exc


BAUD = 921_600
PKT_HEADER = 0x56
CMD_HEADER = 0xAB
PKT_LEN = 6
CMD_STATUS = 0xA5
SAMPLES_PER_BATCH = 30

PKT_NAMES = {
    0x00: "DATA",
    0x01: "HEARTBEAT",
    0x07: "ACK",
    0xFC: "LATENCY",
    0xFD: "STATUS",
    0xFE: "READY",
}

MASTER_STATES = {
    0: "IDLE",
    1: "ARMING",
    2: "ARMED",
    3: "RUNNING",
    4: "STOPPING",
    5: "DUMPING",
    6: "PRESTART",
    7: "SCOPE_MULTI",
}


def encode_std(cmd: int, param: int = 0) -> bytes:
    cmd &= 0xFF
    param &= 0xFF
    return bytes([CMD_HEADER, cmd, param, (cmd ^ param) & 0xFF])


def signed24(b2: int, b1: int, b0: int) -> int:
    raw = (b2 << 16) | (b1 << 8) | b0
    return raw - 0x1000000 if raw & 0x800000 else raw


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    return ser


def extract_packets(buf: bytearray) -> list[bytes]:
    packets: list[bytes] = []
    i = 0
    while i < len(buf):
        if buf[i] != PKT_HEADER:
            i += 1
            continue
        if i + PKT_LEN > len(buf):
            break
        packets.append(bytes(buf[i : i + PKT_LEN]))
        i += PKT_LEN
    del buf[:i]
    return packets


def describe(pkt: bytes) -> str:
    node, typ, b2, b1, b0 = pkt[1], pkt[2], pkt[3], pkt[4], pkt[5]
    name = PKT_NAMES.get(typ, f"0x{typ:02X}")
    if typ == 0x00:
        return f"node={node:3d} type={name:9s} sample={signed24(b2, b1, b0)}"
    if typ == 0x01:
        state = MASTER_STATES.get(b0, f"?{b0}")
        return f"node={node:3d} type={name:9s} pga={b2} vdac={b1} state={state}"
    if typ == 0x07:
        return f"node={node:3d} type={name:9s} ack_cmd=0x{b2:02X} val={b1}"
    if typ == 0xFC:
        us = (b2 << 16) | (b1 << 8) | b0
        return f"node={node:3d} type={name:9s} latency_us={us}"
    if typ == 0xFD and node == 0xFF:
        return f"node=255 type={name:9s} master espnow_ok={b2} ap_ch={b1}"
    if typ == 0xFD:
        if b2 == 0x01:
            return f"node={node:3d} type={name:9s} hello psoc_ok={bool(b1)} fs={b0 * 100}Hz"
        if b2 in (0x02, 0x03, 0x04):
            return f"node={node:3d} type={name:9s} mac_part={b2:02X} bytes={b1:02X}:{b0:02X}"
        return f"node={node:3d} type={name:9s} b={b2:02X} {b1:02X} {b0:02X}"
    if typ == 0xFE:
        return f"node={node:3d} type={name:9s} n_slaves={b2}"
    return f"node={node:3d} type={name:9s} bytes={b2:02X} {b1:02X} {b0:02X}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Sniff ESP32 master USB binary packets")
    parser.add_argument("--port", default="COM8", help="Serial port (default: COM8)")
    parser.add_argument("--baud", type=int, default=BAUD, help=f"Baud rate (default: {BAUD})")
    parser.add_argument("--duration", type=float, default=30.0, help="Seconds to read")
    parser.add_argument("--data-every", type=int, default=200, help="Print one DATA packet every N samples")
    parser.add_argument("--summary-every", type=float, default=1.0, help="Summary period in seconds")
    parser.add_argument("--status-probe", action="store_true", help="Send one safe STATUS request after opening")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    print(
        f"[usb] Opening {args.port} @ {args.baud}. "
        "This may reset the ESP32; COM8 carries binary packets, not text logs.",
        flush=True,
    )

    ser = open_port(args.port, args.baud)
    try:
        if args.status_probe:
            time.sleep(0.5)
            ser.write(encode_std(CMD_STATUS, 0))
            print("[usb] Sent STATUS probe", flush=True)

        t0 = time.monotonic()
        next_summary = t0 + max(0.2, args.summary_every)
        deadline = t0 + max(0.1, args.duration)
        buf = bytearray()
        counts = collections.Counter()
        data_by_node = collections.Counter()
        last_state = "?"
        data_total = 0

        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)

            for pkt in extract_packets(buf):
                typ = pkt[2]
                counts[typ] += 1
                if typ == 0x00:
                    data_total += 1
                    data_by_node[pkt[1]] += 1
                    if data_total <= 5 or (args.data_every > 0 and data_total % args.data_every == 0):
                        print(f"[{time.monotonic() - t0:7.2f}s] {describe(pkt)}", flush=True)
                else:
                    if typ == 0x01:
                        last_state = MASTER_STATES.get(pkt[5], f"?{pkt[5]}")
                    print(f"[{time.monotonic() - t0:7.2f}s] {describe(pkt)}", flush=True)

            now = time.monotonic()
            if now >= next_summary:
                parts = []
                for node, samples in sorted(data_by_node.items()):
                    parts.append(f"S{node}:{samples} smp/{samples // SAMPLES_PER_BATCH} bat")
                detail = " ".join(parts) if parts else "no DATA"
                print(
                    f"[{now - t0:7.2f}s] summary state={last_state} "
                    f"DATA={data_total} {detail}",
                    flush=True,
                )
                next_summary = now + max(0.2, args.summary_every)

        names = {PKT_NAMES.get(k, f"0x{k:02X}"): v for k, v in counts.items()}
        print(f"[usb] done counts={names}", flush=True)
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
