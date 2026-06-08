// protocol.js — decode 6-byte master->browser packets, encode browser->master
// commands as the flat JSON the firmware's web_relay.h expects.
// Mirrors python/geophone_scope/protocol.py — keep field names/semantics in sync.

import * as cfg from './config.js';

/** Parsed 6-byte packet from the master (0x56 header). Mirrors protocol.Packet. */
export class Packet {
  constructor(nodeId, ptype, b2, b1, b0) {
    this.nodeId = nodeId;
    this.ptype = ptype;
    this.b2 = b2;
    this.b1 = b1;
    this.b0 = b0;

    const raw = (b2 << 16) | (b1 << 8) | b0;
    this.value24u = raw >>> 0;
    // Sign-extend 24-bit
    this.value24 = (raw & 0x80_0000) ? raw - 0x100_0000 : raw;
  }

  get isData() { return this.ptype === cfg.PTYPE_DATA; }
  get isHeartbeat() { return this.ptype === cfg.PTYPE_HEARTBEAT; }
  get isAck() { return this.ptype === cfg.PTYPE_ACK; }
  get isReady() { return this.ptype === cfg.PTYPE_READY; }
  get isLatency() { return this.ptype === cfg.PTYPE_LATENCY; }
  get isStatus() { return this.ptype === cfg.PTYPE_STATUS; }

  // Heartbeat fields
  get hbPga() { return this.b2; }
  get hbVdac() { return this.b1; }
  get hbMasterState() { return this.b0; }

  // ACK fields
  get ackCmd() { return this.b2; }
  get ackVal() { return this.b1; }

  // READY
  get readyNSlaves() { return this.b2; }

  // STATUS / HELLO
  get statusIsMaster() { return this.nodeId === 0xFF; }
  get statusEspnowOk() { return this.b2; }
  get statusApCh() { return this.b1; }
  get helloPsocOk() { return !!this.b1; }
  /** Sample rate reported in a slave HELLO (b0 = fs/100 Hz, 0 = unknown). */
  get helloFsHz() { return this.b0 * 100; }
  get helloMacSub() { return this.b2; }
  get helloMacHi() { return this.b1; }
  get helloMacLo() { return this.b0; }
}

/** Decode exactly 6 bytes (Uint8Array/Array-like) into a Packet, or null. */
export function decodePacket(buf) {
  if (buf.length < cfg.PKT_LEN) return null;
  if (buf[0] !== cfg.PKT_HEADER) return null;
  return new Packet(buf[1], buf[2], buf[3], buf[4], buf[5]);
}

// ── Encoding (browser -> master) ─────────────────────────────────────────
// The firmware's web_relay.h decodes flat JSON text frames — see that file's
// header comment for the exact schema. "cmd"/"sub" carry the command byte as
// a two-digit hex string (e.g. "A3"), matching config.py's CMD_*/SUBCMD_* names.

const hex2 = (b) => b.toString(16).toUpperCase().padStart(2, '0');

/** Standard command: {cmd, param}. Mirrors protocol.encode_std. */
export function encodeStd(cmd, param) {
  return JSON.stringify({ cmd: hex2(cmd & 0xFF), param: param & 0xFF });
}

/** Set-N (16-bit) command: {cmd, value}. Mirrors protocol.encode_std16. */
export function encodeStd16(cmd, value16) {
  return JSON.stringify({ cmd: hex2(cmd & 0xFF), value: value16 & 0xFFFF });
}

/** Directed slave command: {cmd:"BD", node, sub, param}. Mirrors protocol.encode_directed. */
export function encodeDirected(nodeId, subCmd, param) {
  return JSON.stringify({
    cmd: hex2(cfg.CMD_DIRECTED),
    node: nodeId & 0xFF,
    sub: hex2(subCmd & 0xFF),
    param: param & 0xFF,
  });
}
