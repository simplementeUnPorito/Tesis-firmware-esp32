// Tab "Enlace": configura y opera el modo ENLACE del maestro.
//
// El maestro alterna entre dos fases (ver PLAN_CONECTIVIDAD_MASTER.md):
//   CAPTURA  AP propio en canal 1 + ESP-NOW con los esclavos + esta SPA.
//   ENLACE   se asocia como STA a una red con internet y expone la cola de
//            capturas para que el cliente adquisidor se las lleve al servidor.
//
// Importante: durante ENLACE el maestro NO escucha a los esclavos, y ademas
// cambia de red — o sea que esta misma pagina se queda sin servidor si el
// navegador estaba conectado al AP del maestro. Por eso el estado se consulta
// bajo demanda y los errores de fetch se muestran como "sin respuesta" en vez
// de romper la UI: que no conteste es el resultado esperado de "Conectar ahora"
// cuando uno esta del lado del AP.

const $ = (id) => document.getElementById(id);

// El status del maestro es texto plano clave=valor (link_mode.h: linkStatusText).
// No se uso JSON ahi para no sumar ArduinoJson al firmware por cinco campos.
function parseStatus(text) {
  const out = {};
  for (const line of text.split('\n')) {
    const i = line.indexOf('=');
    if (i > 0) out[line.slice(0, i)] = line.slice(i + 1);
  }
  return out;
}

const PHASE_LABEL = {
  captura: 'en captura (ESP-NOW activo)',
  pedido: 'cambiando de fase…',
  conectando: 'asociándose a la red…',
  sirviendo: 'enlazado — cola disponible',
  volviendo: 'volviendo a captura…',
};

export function initEnlaceTab(log) {
  const statusEl = $('enlace-status');
  const queueEl = $('enlace-queue');
  if (!statusEl) return;   // tab ausente (firmware viejo / index parcial)

  let lastIp = '';

  const show = (msg) => { statusEl.textContent = msg; };

  function render(st) {
    const phase = st.phase || '?';
    const files = Number(st.queue_files || 0);
    const bytes = Number(st.queue_bytes || 0);
    const kb = (bytes / 1024).toFixed(1);
    const freeKb = (Number(st.fs_free || 0) / 1024).toFixed(0);

    let txt = PHASE_LABEL[phase] || phase;
    txt += ` · cola: ${files} archivo${files === 1 ? '' : 's'} (${kb} kB)`;
    txt += ` · libre: ${freeKb} kB`;
    if (st.ip) { txt += ` · IP ${st.ip}`; lastIp = st.ip; }
    if (st.served && st.served !== '0') txt += ` · entregados: ${st.served}`;
    if (st.last_error) txt += ` · último error: ${st.last_error}`;
    show(txt);

    // Cuando el maestro se fue a la otra red, esta pagina deja de alcanzarlo:
    // dejar a mano la URL para retomar desde el lado del cliente.
    if (phase === 'sirviendo' && st.ip) {
      queueEl.innerHTML =
        `<div class="status-text">Desde el cliente (misma red que el maestro):<br>` +
        `<code>http://${st.ip}/enlace/queue</code></div>`;
    }
  }

  async function refresh() {
    try {
      const r = await fetch('/enlace/status', { cache: 'no-store' });
      render(parseStatus(await r.text()));
    } catch (e) {
      show(lastIp
        ? `sin respuesta — el maestro cambió de red. Probá http://${lastIp}/enlace/status`
        : 'sin respuesta del maestro');
    }
  }

  async function loadInto() {
    try {
      const r = await fetch('/enlace/status', { cache: 'no-store' });
      const st = parseStatus(await r.text());
      if (st.ssid && !$('enlace-ssid').value) $('enlace-ssid').value = st.ssid;
      if (st.site && !$('enlace-site').value) $('enlace-site').value = st.site;
      const mm = Number(st.distance_mm || 0);
      if (mm && !$('enlace-dist').value) $('enlace-dist').value = (mm / 1000).toString();
      $('enlace-auto').checked = st.auto === '1';
      render(st);
    } catch (e) {
      show('sin respuesta del maestro');
    }
  }

  $('btn-enlace-save').addEventListener('click', async () => {
    const body = new URLSearchParams();
    body.append('ssid', $('enlace-ssid').value.trim());
    body.append('pass', $('enlace-pass').value);
    body.append('site', $('enlace-site').value.trim());
    const m = parseFloat($('enlace-dist').value);
    body.append('distance_mm', Number.isFinite(m) ? Math.round(m * 1000) : 0);
    body.append('auto', $('enlace-auto').checked ? 1 : 0);
    try {
      const r = await fetch('/enlace/config', { method: 'POST', body });
      render(parseStatus(await r.text()));
      log && log('Enlace: configuración guardada');
    } catch (e) {
      show('no se pudo guardar: ' + e);
    }
  });

  $('btn-enlace-now').addEventListener('click', async () => {
    log && log('Enlace: cambiando de fase — el maestro deja de escuchar a los esclavos');
    try {
      const r = await fetch('/enlace/now', { method: 'POST' });
      const txt = await r.text();
      if (!r.ok) { show('rechazado: ' + txt.split('\n')[0]); return; }
      render(parseStatus(txt));
      // El maestro tarda en asociarse y en el camino baja el AP: esta pagina
      // puede quedarse sin servidor. Se reintenta un par de veces por si el
      // navegador esta del lado de la red nueva.
      setTimeout(refresh, 3000);
      setTimeout(refresh, 9000);
    } catch (e) {
      show('sin respuesta (esperable si estabas en el AP del maestro)');
    }
  });

  $('btn-enlace-done').addEventListener('click', async () => {
    try {
      const r = await fetch('/enlace/done', { method: 'POST' });
      render(parseStatus(await r.text()));
      log && log('Enlace: volviendo a captura');
    } catch (e) {
      show('sin respuesta del maestro');
    }
  });

  // Refrescar al abrir el tab, no en bucle: en fase CAPTURA el dato no cambia
  // y no tiene sentido meter trafico mientras se esta midiendo.
  const btn = $('tab-btn-enlace');
  if (btn) btn.addEventListener('click', loadInto);
  loadInto();
}
