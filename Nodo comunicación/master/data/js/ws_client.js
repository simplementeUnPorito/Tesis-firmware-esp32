// ws_client.js — owns the WebSocket connection to the master, decodes binary
// packet frames, and auto-reconnects with back-off (phones drop WiFi on
// screen-lock, so this must be resilient). Mirrors the role of serial_worker.py
// (packet_received / connection_changed / log_message signals) using
// CustomEvents instead of Qt signals.

import { decodePacket } from './protocol.js';
import { PKT_LEN, PKT_HEADER } from './config.js?v=field-loop-21';

const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 15000;
const SHORT_CLOSE_MS = 1500;
const SHORT_CLOSE_LIMIT = 3;
const STABLE_OPEN_MS = 3000;
const WS_CLOSE_POLICY = 1008;

export class WsClient extends EventTarget {
  constructor(url) {
    super();
    this._url = url;
    this._ws = null;
    this._wantConnected = false;
    this._reconnectMs = RECONNECT_MIN_MS;
    this._reconnectTimer = null;
    this._openMs = null;
    this._stableTimer = null;
    this._shortCloseCount = 0;
    this._blocked = false;
  }

  /** Start connecting (and keep reconnecting until stop() is called). */
  start() {
    if (this._reconnectTimer) { clearTimeout(this._reconnectTimer); this._reconnectTimer = null; }
    if (this._blocked) {
      this._blocked = false;
      this._shortCloseCount = 0;
      this._reconnectMs = RECONNECT_MIN_MS;
    }
    this._wantConnected = true;
    this._open();
  }

  /** Stop and close the connection; no further reconnect attempts. */
  stop() {
    this._wantConnected = false;
    if (this._reconnectTimer) { clearTimeout(this._reconnectTimer); this._reconnectTimer = null; }
    if (this._stableTimer) { clearTimeout(this._stableTimer); this._stableTimer = null; }
    if (this._ws) { this._ws.close(); this._ws = null; }
  }

  get connected() { return !!this._ws && this._ws.readyState === WebSocket.OPEN; }

  /** Send a pre-encoded JSON command string (see protocol.js encode*). */
  send(jsonStr) {
    if (this.connected) this._ws.send(jsonStr);
    else this._log(`WS: not connected, dropping ${jsonStr}`);
  }

  // ── Internal ────────────────────────────────────────────────────────────

  _open() {
    if (this._blocked) return;

    if (this._ws) {
      const state = this._ws.readyState;
      if (state === WebSocket.OPEN || state === WebSocket.CONNECTING) return;
      if (state === WebSocket.CLOSING) {
        this._scheduleReconnect();
        return;
      }
    }

    // Drop a stale CLOSED object without creating a second live socket.
    if (this._ws) {
      const stale = this._ws;
      this._ws = null;
      stale.onopen = null;
      stale.onclose = null;
      stale.onerror = null;
      stale.onmessage = null;
      try { stale.close(); } catch (_) {}
    }
    this._log(`WS: connecting to ${this._url}…`);
    const ws = new WebSocket(this._url);
    ws.binaryType = 'arraybuffer';
    this._ws = ws;

    ws.onopen = () => {
      this._openMs = Date.now();
      this._log('WS: connected');
      this._emitConnection(true);
      if (this._stableTimer) clearTimeout(this._stableTimer);
      this._stableTimer = setTimeout(() => {
        if (this._ws === ws && ws.readyState === WebSocket.OPEN) {
          this._shortCloseCount = 0;
          this._reconnectMs = RECONNECT_MIN_MS;
        }
      }, STABLE_OPEN_MS);
    };

    ws.onclose = (ev) => {
      if (this._ws === ws) this._ws = null;
      if (this._stableTimer) { clearTimeout(this._stableTimer); this._stableTimer = null; }

      const ageMs = this._openMs ? Date.now() - this._openMs : 0;
      const closedFast = ageMs === 0 || ageMs < SHORT_CLOSE_MS;
      const reason = `${ev?.reason || ''}`.toLowerCase();
      const busy = ev?.code === WS_CLOSE_POLICY || reason.includes('busy') || reason.includes('ws_lock');

      if (busy) {
        this._blocked = true;
        this._wantConnected = false;
        this._openMs = null;
        this._emitConnection(false);
        this._log('WS: ocupado por otra pagina/cliente; reconexion automatica detenida');
        return;
      }

      if (closedFast) this._shortCloseCount++;
      else {
        this._shortCloseCount = 0;
        this._reconnectMs = RECONNECT_MIN_MS;
      }

      this._openMs = null;
      this._emitConnection(false);

      if (this._shortCloseCount >= SHORT_CLOSE_LIMIT) {
        this._blocked = true;
        this._wantConnected = false;
        this._log('WS: cierres rapidos repetidos; reconexion automatica detenida');
        return;
      }

      if (this._wantConnected) this._scheduleReconnect();
    };

    ws.onerror = () => {
      // onclose fires right after — reconnect handled there.
    };

    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') {
        this._log(`WS text: ${ev.data}`);
        try {
          const msg = JSON.parse(ev.data);
          if (msg?.type === 'busy') {
            this._blocked = true;
            this._wantConnected = false;
          }
        } catch (_) {}
        return;
      }
      this._handleBinary(new Uint8Array(ev.data));
    };
  }

  _scheduleReconnect() {
    if (this._blocked || this._reconnectTimer) return;
    this._log(`WS: reconnecting in ${(this._reconnectMs / 1000).toFixed(1)}s…`);
    this._reconnectTimer = setTimeout(() => {
      this._reconnectTimer = null;
      this._reconnectMs = Math.min(this._reconnectMs * 2, RECONNECT_MAX_MS);
      if (this._wantConnected) this._open();
    }, this._reconnectMs);
  }

  /** A WS binary frame may carry one or several back-to-back 6-byte packets. */
  _handleBinary(bytes) {
    let i = 0;
    while (i + PKT_LEN <= bytes.length) {
      if (bytes[i] !== PKT_HEADER) { i++; continue; }
      const pkt = decodePacket(bytes.subarray(i, i + PKT_LEN));
      if (pkt) this._emitPacket(pkt);
      i += PKT_LEN;
    }
  }

  _emitPacket(pkt) {
    this.dispatchEvent(new CustomEvent('packet', { detail: pkt }));
  }

  _emitConnection(isConnected) {
    this.dispatchEvent(new CustomEvent('connection', { detail: isConnected }));
  }

  _log(msg) {
    this.dispatchEvent(new CustomEvent('log', { detail: msg }));
  }
}
