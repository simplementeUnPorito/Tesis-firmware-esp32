"""End-to-end capture probe: connect, ARM, SET_RECLEN, trigger VER on slave 1,
and keep reconnecting (like ws_client.js does) so we can see whether the
beacon-pause disconnect happens, how long it lasts, and whether any DUMP/data
packets ever make it back to a web client.
"""
import socket
import base64
import hashlib
import struct
import time
import sys
import json
import argparse
import math

HOST = "192.168.4.1"
PORT = 80
PATH = "/ws"
TOTAL_S = 45
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
SAFE_DEFAULT_BATCHES = 34
LONG_OUTAGE_RISK_BATCHES = 150
MAX_BATCHES = 512

# Matches config.js PTYPE_* exactly (my earlier guesses here were wrong).
PKT_NAMES = {
    0x00: "DATA", 0x01: "HEARTBEAT", 0x07: "ACK",
    0xFC: "LATENCY", 0xFD: "STATUS", 0xFE: "READY",
}
MASTER_STATES = ["IDLE", "ARMING", "ARMED", "RUNNING", "STOPPING", "DUMPING", "PRESTART", "SCOPE_MULTI"]


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
    frame = bytes([0x81, 0x80 | len(payload)]) + mask + payload
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
    if typ == 0x01:  # heartbeat: b0 = g_state
        st = MASTER_STATES[b0] if b0 < len(MASTER_STATES) else f"?{b0}"
        extra = f"  [state={st}]"
    elif typ == 0x07:  # ack: b2=cmd/sub, b1=value
        extra = f"  [ack cmd=0x{b2:02X} val={b1}]"
    elif typ == 0xFE:  # ready: b2 = n_slaves
        extra = f"  [n_slaves={b2}]"
    return f"node={node:3d} type={name:9s} bytes={b2:02X} {b1:02X} {b0:02X}{extra}"


def iter_packets(payload):
    """Yield 6-byte 0x56 packets from a WS binary message.

    The browser client accepts one or several packets per binary frame; keep
    this probe aligned with that behavior so firmware batching is testable.
    """
    i = 0
    while i + 6 <= len(payload):
        if payload[i] != 0x56:
            i += 1
            continue
        yield payload[i:i + 6]
        i += 6


def parse_args():
    parser = argparse.ArgumentParser(description="Safe WS VER capture probe")
    parser.add_argument(
        "-n", "--batches",
        type=int,
        default=SAFE_DEFAULT_BATCHES,
        help=f"SET_RECLEN batches to request (default: {SAFE_DEFAULT_BATCHES})",
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=None,
        help="Capture duration to convert to batches with ceil(seconds*fs/30)",
    )
    parser.add_argument(
        "--fs",
        type=float,
        default=2929.0,
        help="Sample rate for --seconds conversion (default: 2929 Hz)",
    )
    parser.add_argument(
        "--allow-long",
        action="store_true",
        help=f"Allow values >= {LONG_OUTAGE_RISK_BATCHES}; may drop WiFi association",
    )
    parser.add_argument(
        "--total",
        type=int,
        default=TOTAL_S,
        help=f"Total runtime in seconds (default: {TOTAL_S})",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    n_batches = args.batches
    if args.seconds is not None:
        if args.seconds <= 0:
            raise SystemExit("--seconds must be > 0")
        if args.fs <= 0:
            raise SystemExit("--fs must be > 0")
        n_batches = math.ceil(args.seconds * args.fs / 30)

    if n_batches < 1:
        raise SystemExit("--batches must be >= 1")
    if n_batches > MAX_BATCHES:
        raise SystemExit(f"--batches must be <= {MAX_BATCHES}")
    if n_batches >= LONG_OUTAGE_RISK_BATCHES and not args.allow_long:
        raise SystemExit(
            f"--batches {n_batches} may cause long WiFi outage; "
            "use --allow-long only if you accept that risk"
        )

    t0 = time.time()
    log = lambda m: print(f"[{time.time()-t0:7.2f}s] {m}", flush=True)

    sent_arm = sent_reclen = sent_ver = False
    n_connects = 0
    n_data = 0
    last_connect_at = None
    last_disconnect_at = None

    total_s = max(10, args.total)

    while time.time() - t0 < total_s:
        buf = [b""]
        try:
            sock = socket.create_connection((HOST, PORT), timeout=8)
            sock.settimeout(3)
            buf[0] = handshake(sock, b64key())
            n_connects += 1
            last_connect_at = time.time() - t0
            gap = "" if last_disconnect_at is None else f" (disconnected for {last_connect_at - last_disconnect_at:.2f}s)"
            log(f"=== CONNECTED (#{n_connects}){gap} ===")
        except (ConnectionError, OSError) as e:
            log(f"connect failed: {e}; retrying in 1s")
            time.sleep(1)
            continue

        try:
            while time.time() - t0 < total_s:
                # Drive the capture sequence shortly after the first connection.
                el = time.time() - t0
                if not sent_arm and el > 1.5:
                    send_text(sock, {"cmd": "A2", "param": 1})
                    log(">>> sent ARM n=1 (cmd A2)")
                    sent_arm = True
                elif sent_arm and not sent_reclen and el > 3.0:
                    send_text(sock, {"cmd": "AE", "value": n_batches})
                    if args.seconds is None:
                        log(f">>> sent SET_RECLEN={n_batches} (cmd AE)")
                    else:
                        log(f">>> sent SET_RECLEN={n_batches} for {args.seconds:g}s @ {args.fs:g}Hz")
                    sent_reclen = True
                elif sent_reclen and not sent_ver and el > 4.5:
                    send_text(sock, {"cmd": "BD", "node": 1, "sub": "B2", "param": 1})
                    log(">>> sent VER directed to node 1 (cmd BD sub B2) -- WATCH FOR DISCONNECT")
                    sent_ver = True

                try:
                    opcode, payload = read_frame(sock, buf)
                except socket.timeout:
                    continue
                if opcode == 0x2:
                    packets = list(iter_packets(payload))
                    if not packets:
                        log(f"PKT  {payload.hex()}")
                    for pkt in packets:
                        if pkt[2] == 0x00:
                            n_data += 1
                            if n_data <= 5 or n_data % 25 == 0:
                                log(f"PKT  {describe(pkt)}  raw={pkt.hex()}  (#{n_data} DATA so far)")
                        else:
                            log(f"PKT  {describe(pkt)}  raw={pkt.hex()}")
                elif opcode == 0x1:
                    log(f"TEXT {payload!r}")
                elif opcode == 0x8:
                    log("CLOSE frame received")
                    raise ConnectionError("server closed")
        except (ConnectionError, OSError) as e:
            last_disconnect_at = time.time() - t0
            log(f"--- DISCONNECTED: {e} ---")
            time.sleep(1)
        finally:
            try:
                sock.close()
            except Exception:
                pass

    log(f"done. total connects={n_connects} DATA pkts received={n_data}")


if __name__ == "__main__":
    sys.exit(main())
