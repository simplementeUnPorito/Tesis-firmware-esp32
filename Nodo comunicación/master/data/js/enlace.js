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

// La pagina tiene que vivir en geo-obtain.local, no en una IP. Motivo: el maestro habita
// dos redes y el cliente se mueve entre ellas; una pagina cargada desde
// 192.168.4.1 queda con un origen que deja de existir en cuanto el telefono
// vuelve a su red, y hay que recargar a mano en la IP nueva. geo-obtain.local resuelve
// las dos (mDNS del lado del cliente, portal cautivo del lado del AP), asi que el
// origen sobrevive el cambio y el WebSocket se reconecta solo.
function ensureStableOrigin() {
  const h = location.hostname;
  if (!h || h === 'geo-obtain.local' || h === 'geo-obtain') return;
  // Solo redirigir si estamos en una IP: un nombre cualquiera puede ser un proxy
  // o un tunel que el usuario puso a proposito.
  if (!/^\d+\.\d+\.\d+\.\d+$/.test(h)) return;
  location.replace(`http://geo-obtain.local${location.pathname}${location.search}`);
}

export function initEnlaceTab(log) {
  ensureStableOrigin();
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

    // Siempre se nombra geo-obtain.local, en las dos redes: es la unica direccion que
    // hay que recordar. La IP va como dato de respaldo por si mDNS falla (algunos
    // Android no resuelven .local de forma confiable).
    let txt = st.sta === 'up'
      ? `en las dos redes · http://geo-obtain.local (${st.ip} en ${st.ssid || 'la red'})`
      : `solo en GeoNetwork · http://geo-obtain.local — sin conectar a ${st.ssid || '(sin red cargada)'}`;
    // El canal es el dato que decide si el maestro puede estar en las dos redes
    // a la vez: los esclavos tienen que estar en ESTE canal para que ESP-NOW
    // siga andando mientras la STA esta asociada.
    const ch = Number(st.channel || 0);
    if (ch) txt += ` · canal ${ch}${ch === 1 ? '' : ' (los esclavos deben seguirlo)'}`;
    if (files > 0) txt += ` · ${files} captura${files === 1 ? '' : 's'} en cola (${kb} kB)`;
    if (st.ip) lastIp = st.ip;
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

  // Escaneo: dice si el maestro VE la red, que es lo primero a descartar cuando
  // no se asocia. Si la red no aparece aca, no es la contrasena: el ESP32 es solo
  // 2.4 GHz, asi que un hotspot en 5 GHz (o WPA3, o oculto) es invisible para el.
  const sleep = (ms) => new Promise((res) => setTimeout(res, ms));

  $('btn-enlace-scan').addEventListener('click', async () => {
    show('escaneando…');
    try {
      // El maestro contesta 202 mientras el escaneo corre (no puede bloquear la
      // task de AsyncTCP), asi que hay que volver a preguntar hasta que termine.
      let txt = '';
      let listo = false;
      for (let intento = 0; intento < 8; intento++) {
        const r = await fetch('/enlace/scan', { cache: 'no-store' });
        if (r.status === 202) { await sleep(1200); continue; }
        if (!r.ok) { show(`el maestro respondió HTTP ${r.status} al escanear`); return; }
        txt = (await r.text()).trim();
        listo = true;
        break;
      }
      if (!listo) { show('el escaneo no terminó a tiempo'); return; }
      const dl = $('enlace-redes');
      dl.innerHTML = '';
      if (!txt) { show('el maestro no ve ninguna red 2.4 GHz'); return; }
      const rows = txt.split('\n').map((l) => {
        const [ch, rssi, ...rest] = l.split(' ');
        return { ch: Number(ch), rssi: Number(rssi), ssid: rest.join(' ') };
      });
      for (const row of rows) {
        const o = document.createElement('option');
        o.value = row.ssid;
        o.label = `canal ${row.ch} · ${row.rssi} dBm`;
        dl.appendChild(o);
      }
      const buscada = $('enlace-ssid').value.trim();
      const hit = rows.find((x) => x.ssid === buscada);
      if (buscada && hit) {
        show(`"${buscada}" visible en canal ${hit.ch} (${hit.rssi} dBm) · ` +
             (hit.ch === 1
               ? 'mismo canal que los esclavos'
               : 'los esclavos tendrán que seguir el canal ' + hit.ch));
      } else if (buscada) {
        show(`"${buscada}" NO aparece entre ${rows.length} redes. ` +
             'Si el hotspot está en 5 GHz o WPA3, el ESP32 no lo ve: pasalo a 2.4 GHz/WPA2.');
      } else {
        show(`${rows.length} redes visibles — elegí una de la lista`);
      }
      log && log(`Enlace: escaneo, ${rows.length} redes`);
    } catch (e) {
      show('no se pudo escanear: ' + e);
    }
  });

  // Refrescar al abrir el tab, no en bucle: en fase CAPTURA el dato no cambia
  // y no tiene sentido meter trafico mientras se esta midiendo.
  const btn = $('tab-btn-enlace');
  if (btn) btn.addEventListener('click', loadInto);
  loadInto();
}
