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
      if (!r.ok) { show(`el maestro respondió HTTP ${r.status} en /enlace/status`); return; }
      const st = parseStatus(await r.text());
      if (st.ssid && !$('enlace-ssid').value) $('enlace-ssid').value = st.ssid;
      render(st);
    } catch (e) {
      show('sin respuesta del maestro');
    }
  }

  // Sin boton de guardar: la red se persiste al salir del campo. Es un dato que
  // se carga una vez y no se vuelve a tocar; un boton "Guardar" solo agrega un
  // paso que se puede olvidar.
  async function saveConfig() {
    const ssid = $('enlace-ssid').value.trim();
    if (!ssid) return;
    const body = new URLSearchParams();
    body.append('ssid', ssid);
    body.append('pass', $('enlace-pass').value);
    // site/distance_mm no se mandan: la geometria (offsets martillo-geofono por
    // nodo) la lleva el ZIP que arma la SPA, que es el camino principal hacia el
    // servidor. Pedirla aca de nuevo era configuracion duplicada.
    try {
      const r = await fetch('/enlace/config', { method: 'POST', body });
      if (!r.ok) { show(`el maestro respondió HTTP ${r.status} al guardar`); return; }
      render(parseStatus(await r.text()));
      log && log(`Enlace: red "${ssid}" guardada`);
    } catch (e) {
      show('no se pudo guardar: ' + e);
    }
  }

  $('enlace-ssid').addEventListener('change', saveConfig);
  $('enlace-pass').addEventListener('change', saveConfig);

  // Refrescar al abrir el tab, no en bucle: en fase CAPTURA el dato no cambia
  // y no tiene sentido meter trafico mientras se esta midiendo.
  const btn = $('tab-btn-enlace');
  if (btn) btn.addEventListener('click', loadInto);
  loadInto();
}
