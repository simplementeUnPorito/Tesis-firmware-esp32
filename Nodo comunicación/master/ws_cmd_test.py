"""WS command/ACK round-trip probe: exercises the short, non-capture commands
(STATUS, ARM, directed PGA/VDAC/PGAVDAC/LATENCY, STOP, STREAM) one at a time
and logs every ACK/STATUS/READY frame that comes back, so we can confirm each
button in the web UI maps to a working round trip WITHOUT ever entering
beacon_pause() (which only fires for START/SCOPE_MULTI/VER captures and is
known to cause long WiFi outages — already reported separately).
"""
import socket
import base64
import struct
import time
import sys
import json

HOST = "192.168.4.1"
PORT = 80
PATH = "/ws"
TOTAL_S = 90

PKT_NAMES = {
    0x00: "DATA", 0x01: "HEARTBEAT", 0x07: "ACK",
    0xFC: "LATENCY", 0xFD: "STATUS", 0xFE: "READY",
}
MASTER_STATES = ["IDLE", "ARMING", "ARMED", "RUNNING", "STOPPING", "DUMPING", "PRESTART", "SCOPE_MULTI"]

# (label, message dict, settle seconds before next)
SEQUENCE = [
    ("STATUS",         {"cmd": "A5", "param": 0},                          1.5),
    ("ARM n=1",        {"cmd": "A2", "param": 1},                          2.5),
    ("PGA node1=3",    {"cmd": "BD", "node": 1, "sub": "A6", "param": 3},  1.5),
    ("VDAC node1=128", {"cmd": "BD", "node": 1, "sub": "AA", "param": 128},1.5),
    ("PGAVDAC node1",  {"cmd": "BD", "node": 1, "sub": "A9", "param": 0x73}, 1.5),
    ("LATENCY probe",  {"cmd": "BD", "node": 1, "sub": "AF", "param": 7},  2.5),
    ("STATUS again",   {"cmd": "A5", "param": 0},                          1.5),
    ("STOP",           {"cmd": "A4", "param": 0},                          2.0),
]


def b64key():
    return base64.b64encode(f"k{time.time()}".encode()[:16]).decode()


def handshake(sock, key):
    req = (f"GET {PATH} HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n")
    sock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("closed during handshake")
        resp += chunk
    if " 101 " not in resp.decode(errors="replace").splitlines()[0]:
        raise ConnectionError("handshake rejected: " + resp.decode(errors="replace").splitlines()[0])
    return resp[resp.index(b"\r\n\r\n") + 4:]


def send_text(sock, obj):
    payload = json.dumps(obj).encode()
    mask = b"\x00\x00\x00\x00"
    if len(payload) < 126:
        frame = bytes([0x81, 0x80 | len(payload)]) + mask + payload
    else:
        frame = bytes([0x81, 0x80 | 126]) + struct.pack(">H", len(payload)) + mask + payload
    sock.sendall(frame)


def recv_exact(sock, n, buf):
    while len(buf[0]) < n:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("closed mid-frame")
        buf[0] += chunk
    d, buf[0] = buf[0][:n], buf[0][n:]
    return d


def read_frame(sock, buf):
    hdr = recv_exact(sock, 2, buf)
    opcode = hdr[0] & 0x0F
    masked = bool(hdr[1] & 0x80)
    plen = hdr[1] & 0x7F
    if plen == 126:
        plen = struct.unpack(">H", recv_exact(sock, 2, buf))[0]
    elif plen == 127:
        plen = struct.unpack(">Q", recv_exact(sock, 8, buf))[0]
    mask = recv_exact(sock, 4, buf) if masked else b""
    payload = recv_exact(sock, plen, buf)
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def describe(pkt):
    if len(pkt) != 6 or pkt[0] != 0x56:
        return pkt.hex()
    node, typ, b2, b1, b0 = pkt[1], pkt[2], pkt[3], pkt[4], pkt[5]
    name = PKT_NAMES.get(typ, f"0x{typ:02X}")
    extra = ""
    if typ == 0x01:
        st = MASTER_STATES[b0] if b0 < len(MASTER_STATES) else f"?{b0}"
        extra = f"  [state={st}]"
    elif typ == 0x07:
        extra = f"  [ack cmd=0x{b2:02X} val={(b1<<8)|b0}]"
    elif typ == 0xFE:
        extra = f"  [n_slaves={b0}]"
    elif typ == 0xFC:
        extra = f"  [latency raw b2={b2} b1={b1} b0={b0}]"
    elif typ == 0xFD:
        extra = f"  [status raw b2={b2:02X} b1={b1:02X} b0={b0:02X}]"
    return f"node={node:3d} type={name:9s} bytes={b2:02X} {b1:02X} {b0:02X}{extra}"


def main():
    t0 = time.time()
    log = lambda m: print(f"[{time.time()-t0:7.2f}s] {m}", flush=True)

    buf = [b""]
    sock = socket.create_connection((HOST, PORT), timeout=8)
    sock.settimeout(0.5)
    buf[0] = handshake(sock, b64key())
    log("=== CONNECTED ===")

    seq_idx = 0
    next_send_at = time.time() + 1.0
    acks_seen = []
    states_seen = []

    try:
        while time.time() - t0 < TOTAL_S:
            if seq_idx < len(SEQUENCE) and time.time() >= next_send_at:
                label, msg, settle = SEQUENCE[seq_idx]
                send_text(sock, msg)
                log(f">>> [{seq_idx+1}/{len(SEQUENCE)}] sent {label}: {msg}")
                next_send_at = time.time() + settle
                seq_idx += 1

            try:
                opcode, payload = read_frame(sock, buf)
            except socket.timeout:
                continue
            if opcode == 0x2:
                d = describe(payload)
                log(f"PKT  {d}  raw={payload.hex()}")
                if len(payload) == 6 and payload[0] == 0x56:
                    typ, b2, b1, b0 = payload[2], payload[3], payload[4], payload[5]
                    if typ == 0x07:
                        acks_seen.append((time.time()-t0, b2, (b1 << 8) | b0))
                    elif typ == 0x01:
                        st = MASTER_STATES[b0] if b0 < len(MASTER_STATES) else f"?{b0}"
                        if not states_seen or states_seen[-1][1] != st:
                            states_seen.append((time.time()-t0, st))
            elif opcode == 0x1:
                log(f"TEXT {payload!r}")
            elif opcode == 0x8:
                log("CLOSE frame received")
                raise ConnectionError("server closed")
    except (ConnectionError, OSError) as e:
        log(f"--- DISCONNECTED: {e} ---")
    finally:
        try:
            sock.close()
        except Exception:
            pass

    log("")
    log(f"=== SUMMARY: {len(acks_seen)} ACK(s) received, {len(states_seen)} state transition(s) ===")
    for t, cmd, val in acks_seen:
        log(f"  ACK @ {t:6.2f}s  cmd=0x{cmd:02X} val={val}")
    for t, st in states_seen:
        log(f"  STATE @ {t:6.2f}s -> {st}")
    sent_labels = [s[0] for s in SEQUENCE[:seq_idx]]
    log(f"  commands sent (in order): {sent_labels}")


if __name__ == "__main__":
    sys.exit(main())
