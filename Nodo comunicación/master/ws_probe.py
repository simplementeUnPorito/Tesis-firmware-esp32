"""Minimal stdlib-only WS probe against the master's /ws endpoint.

Connects, stays open, logs every received frame (with timestamps) and
any disconnect, so we can see *when* and *how often* the link drops —
without needing a phone or browser. Run for ~90s while idle (no capture).
"""
import socket
import base64
import hashlib
import struct
import time
import sys

HOST = "192.168.4.1"
PORT = 80
PATH = "/ws"
DURATION_S = 90

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_handshake(sock):
    key = base64.b64encode(b"probe-key-0123456789").decode()
    req = (
        f"GET {PATH} HTTP/1.1\r\n"
        f"Host: {HOST}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("closed during handshake")
        resp += chunk
    head = resp.decode(errors="replace")
    if " 101 " not in head.splitlines()[0]:
        raise ConnectionError(f"handshake rejected: {head.splitlines()[0]}")
    expect = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    if expect not in head:
        raise ConnectionError("Sec-WebSocket-Accept mismatch")
    return resp[resp.index(b"\r\n\r\n") + 4:]


def recv_exact(sock, n, leftover):
    while len(leftover[0]) < n:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("closed mid-frame")
        leftover[0] += chunk
    data, leftover[0] = leftover[0][:n], leftover[0][n:]
    return data


def read_frame(sock, leftover):
    hdr = recv_exact(sock, 2, leftover)
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    masked = bool(b1 & 0x80)
    plen = b1 & 0x7F
    if plen == 126:
        plen = struct.unpack(">H", recv_exact(sock, 2, leftover))[0]
    elif plen == 127:
        plen = struct.unpack(">Q", recv_exact(sock, 8, leftover))[0]
    mask = recv_exact(sock, 4, leftover) if masked else b""
    payload = recv_exact(sock, plen, leftover)
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def main():
    t0 = time.time()
    leftover = [b""]
    sock = socket.create_connection((HOST, PORT), timeout=10)
    sock.settimeout(5)
    try:
        leftover[0] = ws_handshake(sock)
        print(f"[{time.time()-t0:6.2f}s] handshake OK, listening for {DURATION_S}s...")
        last = time.time()
        while time.time() - t0 < DURATION_S:
            try:
                opcode, payload = read_frame(sock, leftover)
            except socket.timeout:
                print(f"[{time.time()-t0:6.2f}s] (no frame in 5s, still connected)")
                continue
            now = time.time()
            gap = now - last
            last = now
            if opcode == 0x2:  # binary
                hexstr = payload.hex()
                print(f"[{now-t0:6.2f}s] +{gap:5.2f}s BINARY {len(payload)}B: {hexstr}")
            elif opcode == 0x1:
                print(f"[{now-t0:6.2f}s] +{gap:5.2f}s TEXT: {payload!r}")
            elif opcode == 0x8:
                print(f"[{now-t0:6.2f}s] +{gap:5.2f}s CLOSE frame: {payload!r}")
                break
            elif opcode == 0x9:
                print(f"[{now-t0:6.2f}s] +{gap:5.2f}s PING")
            elif opcode == 0xA:
                print(f"[{now-t0:6.2f}s] +{gap:5.2f}s PONG")
            else:
                print(f"[{now-t0:6.2f}s] +{gap:5.2f}s opcode=0x{opcode:X} {len(payload)}B")
    except (ConnectionError, OSError) as e:
        print(f"[{time.time()-t0:6.2f}s] DROPPED: {e}")
    finally:
        sock.close()
        print(f"[{time.time()-t0:6.2f}s] socket closed, total runtime {time.time()-t0:.1f}s")


if __name__ == "__main__":
    sys.exit(main())
