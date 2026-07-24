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

// URL del servidor tal como la tiene guardada el maestro. La usa el boton
// "Subir al server" de la pestana Captura: el POST lo hace el navegador, pero la
// direccion se guarda en el maestro para no tener que cargarla en cada cliente.
let cachedServerUrl = '';
export function getServerUrl() { return cachedServerUrl; }

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
  if (!statusEl) return;   // tab ausente (firmware viejo / index parcial)

  let lastIp = '';

  const show = (msg) => { statusEl.textContent = msg; };

  function render(st, raw) {
    // Si el maestro contesta 200 pero sin los campos esperados, mostrar lo que
    // realmente mando: adivinar por que "phase" viene vacio cuesta mas que
    // leerlo. Paso de verdad y sin esto no habia forma de saber que respondia.
    if (!st.phase) {
      show('respuesta inesperada del maestro: ' +
           (raw ? JSON.stringify(raw.slice(0, 120)) : '(vacía)'));
      return;
    }
    const phase = st.phase;
    const files = Number(st.queue_files || 0);
    const bytes = Number(st.queue_bytes || 0);
    const kb = (bytes / 1024).toFixed(1);
    const freeKb = (Number(st.fs_free || 0) / 1024).toFixed(0);

    let txt = PHASE_LABEL[phase] || phase;
    if (files > 0) txt += ` · ${files} captura${files === 1 ? '' : 's'} sin subir (${kb} kB)`;
    if (st.ip) { txt += ` · IP ${st.ip}`; lastIp = st.ip; }
    if (st.last_error) txt += ` · ${st.last_error}`;
    txt += ` · libre ${freeKb} kB`;
    show(txt);
  }

  async function refresh() {
    try {
      const r = await fetch('/enlace/status', { cache: 'no-store' });
      if (!r.ok) { show(`el maestro respondió HTTP ${r.status} en /enlace/status`); return; }
      const txt = await r.text();
      render(parseStatus(txt), txt);
    } catch (e) {
      show(lastIp
        ? `sin respuesta — el maestro cambió de red. Probá http://${lastIp}/enlace/status`
        : 'sin respuesta del maestro');
    }
  }

  async function loadInto() {
    try {
      const r = await fetch('/enlace/status', { cache: 'no-store' });
      if (!r.ok) { show(`el maestro respondió HTTP ${r.status} en /enlace/status`); return; }
      const txt = await r.text();
      const st = parseStatus(txt);
      // Lo guardado en el maestro manda sobre lo que el navegador haya
      // autocompletado: si no, un autofill viejo tapa la config real.
      if (st.ssid) $('enlace-ssid').value = st.ssid;
      if (st.server_url !== undefined) $('enlace-server').value = st.server_url;
      if (st.server_url) cachedServerUrl = st.server_url;
      render(st, txt);
    } catch (e) {
      show('sin respuesta del maestro');
    }
  }

  // Guardado explicito: el auto-guardado al salir del campo no daba ninguna
  // senal de haber pasado, y en un formulario que se toca una vez cada tanto eso
  // se siente como que la pagina ignora lo que escribis.
  async function saveConfig() {
    const ssid = $('enlace-ssid').value.trim();
    const url = $('enlace-server').value.trim();
    if (!ssid) { show('falta el nombre de la red'); return; }
    const body = new URLSearchParams();
    body.append('ssid', ssid);
    body.append('pass', $('enlace-pass').value);
    body.append('server_url', url);
    // site/distance_mm no se mandan: la geometria (offsets martillo-geofono por
    // nodo) la lleva el ZIP que arma la SPA, que es el camino principal hacia el
    // servidor. Pedirla aca de nuevo era configuracion duplicada.
    show('guardando…');
    try {
      const r = await fetch('/enlace/config', { method: 'POST', body });
      const txt = await r.text();
      if (!r.ok) { show(`el maestro respondió HTTP ${r.status} al guardar`); return; }
      cachedServerUrl = url;
      render(parseStatus(txt), txt);
      log && log(`Enlace: guardado (red "${ssid}"${url ? ', servidor ' + url : ''})`);
    } catch (e) {
      show('no se pudo guardar: ' + e);
    }
  }

  $('btn-enlace-save').addEventListener('click', saveConfig);

  // Refrescar al abrir el tab, no en bucle: en fase CAPTURA el dato no cambia
  // y no tiene sentido meter trafico mientras se esta midiendo.
  const btn = $('tab-btn-enlace');
  if (btn) btn.addEventListener('click', loadInto);
  loadInto();
}
