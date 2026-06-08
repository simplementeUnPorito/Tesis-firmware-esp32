// ws_client.js — owns the WebSocket connection to the master, decodes binary
// packet frames, and auto-reconnects with back-off (phones drop WiFi on
// screen-lock, so this must be resilient). Mirrors the role of serial_worker.py
// (packet_received / connection_changed / log_message signals) using
// CustomEvents instead of Qt signals.

import { decodePacket } from './protocol.js';
import { PKT_LEN, PKT_HEADER } from './config.js';

const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 15000;

export class WsClient extends EventTarget {
  constructor(url) {
    super();
    this._url = url;
    this._ws = null;
    this._wantConnected = false;
    this._reconnectMs = RECONNECT_MIN_MS;
    this._reconnectTimer = null;
  }

  /** Start connecting (and keep reconnecting until stop() is called). */
  start() {
    this._wantConnected = true;
    this._open();
  }

  /** Stop and close the connection; no further reconnect attempts. */
  stop() {
    this._wantConnected = false;
    if (this._reconnectTimer) { clearTimeout(this._reconnectTimer); this._reconnectTimer = null; }
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
    this._log(`WS: connecting to ${this._url}…`);
    const ws = new WebSocket(this._url);
    ws.binaryType = 'arraybuffer';
    this._ws = ws;

    ws.onopen = () => {
      this._reconnectMs = RECONNECT_MIN_MS;
      this._log('WS: connected');
      this._emitConnection(true);
    };

    ws.onclose = () => {
      this._emitConnection(false);
      if (this._wantConnected) this._scheduleReconnect();
    };

    ws.onerror = () => {
      // onclose fires right after — reconnect handled there.
    };

    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') {
        this._log(`WS text: ${ev.data}`);
        return;
      }
      this._handleBinary(new Uint8Array(ev.data));
    };
  }

  _scheduleReconnect() {
    if (this._reconnectTimer) return;
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
