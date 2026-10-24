// Actualizacion de firmware de los esclavos (OTA): sube el .bin al maestro,
// lo ofrece a un nodo o a todos y muestra el avance que informa cada esclavo.

const STATE_NAMES = ['inactivo', 'asociandose', 'descargando', 'verificando',
  'reiniciando', 'sin confirmar', 'confirmado', 'falló'];
const ERROR_NAMES = ['', 'captura en curso', 'no se asoció al AP', 'error HTTP',
  'tamaño distinto', 'SHA-256 distinto', 'error de flash', 'abortado'];

export function mountOtaPanel(container, log = () => {}) {
  if (!container || document.getElementById('ota-panel')) return;
  const box = document.createElement('fieldset');
  box.id = 'ota-panel';
  box.className = 'panel';
  box.innerHTML = `
    <legend>Actualizar firmware de esclavos (OTA)</legend>
    <div class="row">
      <input type="file" id="ota-file" accept=".bin">
      <button type="button" id="ota-upload">Subir al maestro</button>
      <span id="ota-image" class="muted">sin imagen</span>
    </div>
    <div class="row">
      <label>Nodo <select id="ota-node"><option value="0">todos los vistos</option></select></label>
      <button type="button" id="ota-start">Actualizar</button>
      <button type="button" id="ota-abort">Abortar</button>
    </div>
    <table id="ota-table" class="small"><thead><tr>
      <th>Nodo</th><th>Estado</th><th>Avance</th><th>Error</th><th>Hace</th>
    </tr></thead><tbody></tbody></table>
    <p class="muted">Fuera de captura. El esclavo baja la imagen por el WiFi del maestro,
    verifica el SHA-256 y reinicia; si después no vuelve a hablar con el maestro,
    regresa solo a la versión anterior.</p>`;
  container.prepend(box);

  const nodeSel = box.querySelector('#ota-node');
  for (let n = 1; n <= 8; n++) {
    const o = document.createElement('option');
    o.value = String(n); o.textContent = `S${n}`;
    nodeSel.appendChild(o);
  }

  box.querySelector('#ota-upload').addEventListener('click', async () => {
    const f = box.querySelector('#ota-file').files?.[0];
    if (!f) { log('OTA: elegí un archivo .bin'); return; }
    const fd = new FormData();
    fd.append('file', f, f.name);
    log(`OTA: subiendo ${f.name} (${f.size} bytes)…`);
    try {
      const r = await fetch('/ota/upload', { method: 'POST', body: fd });
      const j = await r.json();
      if (!j.ok) log(`OTA: rechazado: ${j.error}`);
      else log(`OTA: imagen lista ${j.size} bytes sha256 ${j.sha256.slice(0, 16)}…`);
    } catch (e) { log(`OTA: error al subir: ${e}`); }
    refresh();
  });

  box.querySelector('#ota-start').addEventListener('click', async () => {
    try {
      const r = await fetch(`/ota/start?node=${nodeSel.value}`, { method: 'POST' });
      const j = await r.json();
      log(j.ok ? `OTA: ofrecida a ${j.offered} nodo(s)` : `OTA: no se pudo: ${j.error || 'sin nodos'}`);
    } catch (e) { log(`OTA: ${e}`); }
  });

  box.querySelector('#ota-abort').addEventListener('click', async () => {
    try { await fetch(`/ota/abort?node=${nodeSel.value}`, { method: 'POST' }); log('OTA: aborto pedido'); }
    catch (e) { log(`OTA: ${e}`); }
  });

  const tbody = box.querySelector('#ota-table tbody');
  const imageEl = box.querySelector('#ota-image');
  async function refresh() {
    try {
      const r = await fetch('/ota/status', { cache: 'no-store' });
      const j = await r.json();
      imageEl.textContent = j.image.ok
        ? `imagen: ${j.image.size} bytes · sha256 ${j.image.sha256.slice(0, 16)}…`
        : 'sin imagen';
      tbody.innerHTML = '';
      for (const n of j.nodes) {
        const tr = document.createElement('tr');
        const pct = n.size ? Math.min(100, Math.round(100 * n.received / n.size)) : 0;
        tr.innerHTML = `<td>S${n.node}</td><td>${STATE_NAMES[n.state] ?? n.state}</td>` +
          `<td>${pct}%</td><td>${ERROR_NAMES[n.error] ?? n.error}</td>` +
          `<td>${(n.age_ms / 1000).toFixed(0)} s</td>`;
        tbody.appendChild(tr);
      }
    } catch (_) { /* sin conexion: se reintenta */ }
  }
  refresh();
  setInterval(() => { if (!document.hidden) refresh(); }, 1000);
}
