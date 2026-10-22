# Probe WS rapido y tolerante a takeover: una pestania de navegador ajena
# (misma IP) roba el socket cada ~1-2 s; este probe lo re-roba al instante,
# manda la secuencia ARM/RECLEN/STREAM/VER comprimida y cuenta DATA acumulado
# a traves de reconexiones. Responde PING->PONG y manda keep-alive para que
# webRelayHasActiveClient() no descarte el volcado.
import base64
import json
import socket
import struct
import sys
import time
import urllib.request

HOST = "192.168.4.1"
PORT = 80
PATH = "/ws"
MASTER_STATES = ["IDLE", "ARMING", "ARMED", "RUNNING", "STOPPING", "DUMPING",
                 "PRESTART", "SCOPE_MULTI"]

node = int(sys.argv[1]) if len(sys.argv) > 1 else 2
n_batches = int(sys.argv[2]) if len(sys.argv) > 2 else 2
stream = int(sys.argv[3]) if len(sys.argv) > 3 else 0
total_s = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0

t0 = time.time()
log = lambda m: print(f"[{time.time()-t0:6.2f}s] {m}", flush=True)


def handshake(sock):
    key = base64.b64encode(f"k{time.time()}".encode()[:16]).decode()
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
        raise ConnectionError("handshake rejected")
    return resp[resp.index(b"\r\n\r\n") + 4:]


def send_frame(sock, opcode, payload):
    mask = b"\x00\x00\x00\x00"
    if len(payload) < 126:
        hdr = bytes([0x80 | opcode, 0x80 | len(payload)])
    else:
        hdr = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", len(payload))
    sock.sendall(hdr + mask + payload)


def send_cmd(sock, obj):
    send_frame(sock, 0x1, json.dumps(obj).encode())


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


# Estado de la secuencia (independiente de la conexion actual). VER se
# retiene hasta que el fantasma (pestania ajena) haya robado >=2 veces: cada
# robo re-robado hace crecer su backoff y agranda nuestra ventana de dump.
steps = [
    (0.0, "ARM", {"cmd": "A2", "param": 1}),
    (0.4, "SET_RECLEN", {"cmd": "AE", "value": n_batches}),
    (0.8, "STREAM", {"cmd": "BD", "node": node, "sub": "B7", "param": stream}),
]
VER_CMD = {"cmd": "BD", "node": node, "sub": "B2", "param": 1}
MIN_STEALS_BEFORE_VER = 2
step_i = 0
n_data = 0
n_connects = 0
n_steals = 0
acks = []
last_state = None
seq_start = None
last_keepalive = 0.0
saw_dumping = False
dump_in_flight = False
ver_sent_at = None
connected_at = None
idle_after_dump_at = None
done = False

try:
    urllib.request.urlopen(f"http://{HOST}/ws-reset", timeout=5).read()
    log("ws-reset ok")
except Exception as e:
    log(f"ws-reset fallo: {e}")

while time.time() - t0 < total_s:
    buf = [b""]
    try:
        sock = socket.create_connection((HOST, PORT), timeout=5)
        sock.settimeout(0.2)
        buf[0] = handshake(sock)
        n_connects += 1
        connected_at = time.time()
        if seq_start is None:
            seq_start = time.time()
        log(f"=== conectado #{n_connects} (robos previos={n_steals}) ===")
    except (ConnectionError, OSError) as e:
        log(f"connect fallo: {e}")
        time.sleep(0.2)
        continue

    try:
        while time.time() - t0 < total_s:
            now = time.time()
            el = now - seq_start
            if step_i < len(steps) and el >= steps[step_i][0]:
                _, name, obj = steps[step_i]
                send_cmd(sock, obj)
                log(f">>> {name}")
                step_i += 1
                last_keepalive = now
            elif (step_i >= len(steps) and not dump_in_flight and not done and
                  n_steals >= MIN_STEALS_BEFORE_VER and
                  now - connected_at > 0.4 and n_data < n_batches * 30):
                send_cmd(sock, VER_CMD)
                ver_sent_at = now
                dump_in_flight = True
                log(f">>> VER (intento con ventana tras {n_steals} robos)")
                last_keepalive = now
            elif now - last_keepalive > 1.0:
                send_cmd(sock, {"cmd": "A5", "param": 0})  # STATUS keep-alive
                last_keepalive = now

            try:
                opcode, payload = read_frame(sock, buf)
            except socket.timeout:
                if idle_after_dump_at and time.time() - idle_after_dump_at > 3.0:
                    raise KeyboardInterrupt  # fin feliz
                continue
            if opcode == 0x9:
                send_frame(sock, 0xA, payload)  # PONG
                continue
            if opcode == 0x8:
                raise ConnectionError("server close")
            if opcode != 0x2:
                continue
            i = 0
            while i + 6 <= len(payload):
                if payload[i] != 0x56:
                    i += 1
                    continue
                pkt = payload[i:i + 6]
                i += 6
                typ = pkt[2]
                if typ == 0x00:
                    n_data += 1
                    if n_data == 1 or n_data % 30 == 0:
                        log(f"DATA #{n_data} (node={pkt[1]})")
                    if n_data >= n_batches * 30:
                        done = True
                        if idle_after_dump_at is None:
                            idle_after_dump_at = time.time()
                elif typ == 0x01:
                    st = MASTER_STATES[pkt[5]] if pkt[5] < len(MASTER_STATES) else f"?{pkt[5]}"
                    if st != last_state:
                        log(f"estado: {last_state} -> {st}")
                        if st == "DUMPING":
                            saw_dumping = True
                        if last_state == "DUMPING" and st != "DUMPING":
                            dump_in_flight = False  # dump termino o fue abortado
                        if saw_dumping and st in ("IDLE", "ARMED") and n_data >= n_batches * 30 \
                                and idle_after_dump_at is None:
                            idle_after_dump_at = time.time()
                        last_state = st
                elif typ == 0x07:
                    acks.append((pkt[3], pkt[4]))
                    log(f"ACK cmd=0x{pkt[3]:02X} val={pkt[4]}")
    except KeyboardInterrupt:
        break
    except (ConnectionError, OSError) as e:
        n_steals += 1
        dump_in_flight = False
        log(f"--- desconectado ({e}) — re-robando ---")
    finally:
        try:
            sock.close()
        except Exception:
            pass

expected = n_batches * 30
print(f"\nRESULTADO: connects={n_connects} DATA={n_data}/{expected} "
      f"dumping={'si' if saw_dumping else 'no'} acks={len(acks)}", flush=True)
sys.exit(0 if (saw_dumping and n_data >= expected) else 1)
