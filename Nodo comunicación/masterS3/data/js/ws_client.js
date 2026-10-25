// ws_client.js — owns the WebSocket connection to the master, decodes binary
// packet frames, and auto-reconnects with back-off (phones drop WiFi on
// screen-lock, so this must be resilient). Mirrors the role of serial_worker.py
// (packet_received / connection_changed / log_message signals) using
// CustomEvents instead of Qt signals.

import { decodePacket } from './protocol.js?v=field-study-47';
import { PKT_LEN, PKT_HEADER } from './config.js?v=field-study-47';
import { decodeGeoZ } from './geo_codec.js?v=field-study-47';

const ZBATCH_HEADER = 0x57;

const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 15000;
const BUSY_RETRY_MS = 2500;
const SHORT_CLOSE_MS = 2000;
const SHORT_CLOSE_LIMIT = 5;
const STABLE_OPEN_MS = 5000;
const WS_CLOSE_POLICY = 1008;
const WS_CLOSE_UNAUTH = 4401;

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
    this.authToken = null;  // set from outside if auth is required
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
      // ?takeover=1 vale para la primera conexion: en las reconexiones
      // automaticas dos pestanas se robaban el socket una a la otra.
      this._url = this._url.replace(/[?&]takeover=1$/, '');
      if (this.authToken) {
        ws.send(JSON.stringify({ cmd: 'AUTH', token: this.authToken }));
      }
      ws.send(JSON.stringify({ cmd: 'ZWEB', on: 1 }));
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
      const replaced = this._replaced || reason.includes('replaced');
      this._replaced = false;

      if (replaced) {
        // Otra pestana (o este mismo equipo tras un corte) tomo la conexion:
        // no reintentar solo, para no robarsela de vuelta en bucle.
        this._blocked = true;
        this._openMs = null;
        this._emitConnection(false);
        this._log('WS: otra pestaña de este equipo tomó la conexión; tocá Reconectar para recuperarla');
        return;
      }
      const unauth = ev?.code === WS_CLOSE_UNAUTH || reason.includes('unauthorized') || reason.includes('auth');

      if (busy) {
        this._openMs = null;
        this._emitConnection(false);
        this._reconnectMs = Math.max(this._reconnectMs, BUSY_RETRY_MS);
        this._log('WS: ocupado por otra pagina/cliente; reintentando');
        if (this._wantConnected) this._scheduleReconnect();
        return;
      }

      if (unauth) {
        const wrongPassword = `${ev?.reason || ''}`.toLowerCase().includes('unauthorized');
        this._blocked = true;
        this._wantConnected = false;
        this._openMs = null;
        this._emitConnection(false);
        this._log('WS: autenticacion rechazada; ingresa la contraseña');
        this.dispatchEvent(new CustomEvent('auth-required', {
          detail: { reason: wrongPassword ? 'wrong_password' : 'rejected' },
        }));
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
        this._reconnectMs = RECONNECT_MAX_MS;
        this._shortCloseCount = 0;
        this._log('WS: cierres rapidos repetidos; reintentando en 15 s…');
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
          if (msg?.type === 'replaced') {
            this._replaced = true;
          } else if (msg?.type === 'busy') {
            this._log('WS: maestro ocupado; reintentando');
            this._reconnectMs = Math.max(this._reconnectMs, BUSY_RETRY_MS);
            try { ws.close(); } catch (_) {}
          } else if (msg?.type === 'auth_required') {
            if (!this.authToken) {
              this.dispatchEvent(new CustomEvent('auth-required', { detail: { reason: 'prompt' } }));
            }
          } else if (msg?.type === 'auth') {
            if (!msg.ok) {
              this._log('WS: contraseña incorrecta');
              this.dispatchEvent(new CustomEvent('auth-required', { detail: { reason: 'wrong_password' } }));
            } else {
              ws.send(JSON.stringify({ cmd: 'ZWEB', on: 1 }));
            }
          } else if (msg?.type === 'fill_miss') {
            this.dispatchEvent(new CustomEvent('fill-miss', { detail: msg }));
          } else if (msg?.type === 'ftm') {
            this.dispatchEvent(new CustomEvent('ftm', { detail: msg }));
          } else if (msg?.type === 'sync_start') {
            this.dispatchEvent(new CustomEvent('sync-start', { detail: msg }));
          } else if (msg?.type === 'link') {
            this.dispatchEvent(new CustomEvent('link-rssi', { detail: msg }));
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

  /** Un frame puede mezclar paquetes 0x56 y lotes comprimidos 0x57. */
  _handleBinary(bytes) {
    let i = 0;
    while (i < bytes.length) {
      if (bytes[i] === PKT_HEADER) {
        if (i + PKT_LEN > bytes.length) break;
        const pkt = decodePacket(bytes.subarray(i, i + PKT_LEN));
        if (pkt) this._emitPacket(pkt);
        i += PKT_LEN;
        continue;
      }
      if (bytes[i] === ZBATCH_HEADER) {
        if (i + 6 > bytes.length) break;
        const len = bytes[i + 5];
        if (i + 6 + len > bytes.length) break;
        try {
          const decoded = decodeGeoZ(bytes.subarray(i + 6, i + 6 + len));
          const seq = bytes[i + 2] | (bytes[i + 3] << 8) | (bytes[i + 4] << 16);
          this.dispatchEvent(new CustomEvent('zbatch', {
            detail: { node: bytes[i + 1], seq, values: decoded.values, flags: decoded.flags },
          }));
        } catch (err) {
          this._log(`WS: lote Z invalido (${err?.message || err})`);
        }
        i += 6 + len;
        continue;
      }
      i++;
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
