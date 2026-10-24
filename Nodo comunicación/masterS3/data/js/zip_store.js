// zip_store.js - minimal ZIP writer, store mode only (no compression).

const textEncoder = new TextEncoder();

const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(bytes) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function dosDateTime(date) {
  const year = Math.max(1980, date.getFullYear());
  const dosTime = (date.getHours() << 11) | (date.getMinutes() << 5) | Math.floor(date.getSeconds() / 2);
  const dosDate = ((year - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  return { dosTime, dosDate };
}

function u8(data) {
  if (data instanceof Uint8Array) return data;
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (typeof data === 'string') return textEncoder.encode(data);
  throw new TypeError('Unsupported ZIP payload');
}

function header(size) {
  return new DataView(new ArrayBuffer(size));
}

function blobPart(view) {
  return new Uint8Array(view.buffer);
}

export function buildStoreZip(files) {
  const localParts = [];
  const centralParts = [];
  let offset = 0;
  const now = new Date();
  const { dosTime, dosDate } = dosDateTime(now);

  for (const file of files) {
    const nameBytes = textEncoder.encode(file.name.replaceAll('\\', '/'));
    const data = u8(file.data);
    const crc = crc32(data);

    const local = header(30);
    local.setUint32(0, 0x04034B50, true);
    local.setUint16(4, 20, true);
    local.setUint16(6, 0x0800, true); // UTF-8 names
    local.setUint16(8, 0, true);      // store mode
    local.setUint16(10, dosTime, true);
    local.setUint16(12, dosDate, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, data.byteLength, true);
    local.setUint32(22, data.byteLength, true);
    local.setUint16(26, nameBytes.byteLength, true);
    local.setUint16(28, 0, true);

    localParts.push(blobPart(local), nameBytes, data);

    const central = header(46);
    central.setUint32(0, 0x02014B50, true);
    central.setUint16(4, 20, true);
    central.setUint16(6, 20, true);
    central.setUint16(8, 0x0800, true);
    central.setUint16(10, 0, true);
    central.setUint16(12, dosTime, true);
    central.setUint16(14, dosDate, true);
    central.setUint32(16, crc, true);
    central.setUint32(20, data.byteLength, true);
    central.setUint32(24, data.byteLength, true);
    central.setUint16(28, nameBytes.byteLength, true);
    central.setUint16(30, 0, true);
    central.setUint16(32, 0, true);
    central.setUint16(34, 0, true);
    central.setUint16(36, 0, true);
    central.setUint32(38, 0, true);
    central.setUint32(42, offset, true);
    centralParts.push(blobPart(central), nameBytes);

    offset += local.byteLength + nameBytes.byteLength + data.byteLength;
  }

  const centralOffset = offset;
  const centralSize = centralParts.reduce((sum, part) => sum + part.byteLength, 0);
  const end = header(22);
  end.setUint32(0, 0x06054B50, true);
  end.setUint16(8, files.length, true);
  end.setUint16(10, files.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, centralOffset, true);

  return new Blob([...localParts, ...centralParts, blobPart(end)], { type: 'application/zip' });
}
