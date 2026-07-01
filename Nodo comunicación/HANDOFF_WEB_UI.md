# HANDOFF: Web UI — Estudio de Campo MASW

Sesión: 2026-06-30  
Estado general: **implementado y verificado en hardware para S2 GEO**

Bitácora de pruebas de campo: `master/WEB_FIELD_TESTS.md`, sección
`Session log — 2026-06-30 Codex handoff continuation`.

## Objetivo
Preparar la interfaz web del nodo maestro ESP32 para trabajo en campo MASW. Sin necesidad de recompilar firmware para ajustes de display.

---

## Checklist de cambios

### 1. FIR: eliminar `firFilter` del pipeline en vivo — usar solo `filtFilt`
- [x] **Implementado** — `signal_proc.js`: `firFilter` ya no es export público
- [x] **Implementado** — `app.js` `handleData()`: eliminado bloque firFilter + `filtBuf.push()`
- [x] **Implementado** — `app.js` `renderTick()`: ya usaba `filteredArrayForNode()` con `filtFilt`
- [ ] Probar: señal filtrada debe alinearse con raw sin adelanto/atraso visible

**Archivos:** `master/data/js/signal_proc.js`, `master/data/js/app.js`

### 2. Checkbox "Sin offset DC" en sección de plot (aplica a raw Y filtrada)
- [x] **Implementado** — `index.html`: checkbox `chk-dc-remove` en barra de plot
- [x] **Implementado** — `app.js`: handler global que setea `nd.dcRemove` en todos los nodos + reprocess
- [x] **Implementado** — `app.js` `renderTick()`: aplica `dcRemove()` a rawBufs para display
- [ ] Probar: marcar checkbox → baseline de raw y filtrada cae a cero

**Archivos:** `master/data/index.html`, `master/data/js/app.js`

### 3. Espectro: checkbox que reemplaza el plot de tiempo
- [x] **Implementado** — `index.html`: `<button id="btn-spectrum">` → `<input id="chk-spectrum" type="checkbox">`
- [x] **Implementado** — `app.js`: handler `change` en lugar de `click`, toggle CSS
- [x] Probar: marcar → canvas FFT reemplaza tiempo; desmarcar → vuelve tiempo

**Archivos:** `master/data/index.html`, `master/data/js/app.js`

### 4. Offset Y por canal (separar trazas, solo display)
- [x] **Implementado** — `data_store.js`: campo `yOffsetV = 0` en `NodeData`
- [x] **Implementado** — `slave_panel.js`: input "Offset Y (mV)" + evento `y-offset-changed`
- [x] **Implementado** — `app.js`: listener `y-offset-changed` + aplicación en `renderTick()`
- [x] Probar: subir offset de un canal → traza se desplaza; sin afectar otros

**Archivos:** `master/data/js/data_store.js`, `master/data/js/slave_panel.js`, `master/data/js/app.js`

### 5. Mobile friendliness
- [x] **Implementado** — `style.css`: touch targets (min-height 2.75rem en 520–900px), inputs number, tab font-size 0.85rem, toolbar flex-wrap, sidebar overflow-y auto
- [ ] Probar: abrir desde celular, verificar botones táctiles y sin scroll horizontal

**Archivos:** `master/data/css/style.css`

### 8. Inversión de señal por canal (nuevo — sesión 2)
- [x] **Implementado** — `data_store.js`: campo `invertSignal = false` en `NodeData`
- [x] **Implementado** — `slave_panel.js`: checkbox "Invertir señal" + evento `invert-toggled`
- [x] **Implementado** — `app.js`: listener `invert-toggled` + aplicación en `renderTick()` (×−1 al display)
- [ ] Probar: marcar para Hammer → forma de onda invierte; datos exportados sin cambio

**Nota:** La inversión se aplica junto al yOffset y dcRemove en el mismo loop de display.

### 9. Tipo de esclavo dinámico (GEO/HAMMER desde firmware) + N geos
- [x] **Implementado** — `config.js`: `SLAVE_TYPE_ORDER` generado dinámicamente (Hammer + GeoN para N = MAX_NODES-1)
- [x] **Implementado** — `slave_panel.js`: `setSlaveType(name)` público para seteo desde firmware, `setDisplayName(name)` para actualizar header
- [x] **Implementado** — `slave_panel.js`: ocultar "Offset m" cuando el tipo es Hammer (no es referencia de sí misma)
- [x] **Implementado** — `app.js`: `reorderGeosByOffset()` — ordena geos por distancia y renombra Geo1, Geo2… (Geo1 = más cercano al hammer)
- [x] **Implementado firmware** — PSoC emite `PSOC_HW_CLASS` en `PSOC_EVT_BOOT`; el ESP slave lo reenvía en `MsgHello.hw_class`.
- [x] **Implementado master/web** — master acepta HELLO legacy o nuevo, cachea `hw_class` y lo relaya a WebSocket como status subtype `0x06`.
- [x] **Implementado UI** — `app.js` aplica tipo reportado: `HAMMER` fuerza offset 0 y oculta "Offset m"; `GEO` entra al orden GeoN por distancia.
- [x] Probar físico: S2/COM12 reportó `type=GEO`, `psoc_ok=true`, `fsExact=2929`.
- [x] Probar dummy Hammer: `/sim/hello?node=1&type=hammer&fs=2929&psoc=1` mostró `Hammer (S1)` sin "Offset m".
- [ ] Probar visual final: asignar offsets a dos geos reales/simulados y confirmar relabel Geo1/Geo2 en pantalla. Código ajustado en `field-study-3`; el navegador embebido bloqueó el último reload a `192.168.4.1`, así que quedó para verificación manual rápida.

**Nota:** Los IDs de esclavo (S1, S2...) son estáticos. Solo el label de display cambia con el orden.

### 6. Calibración: tabla VDAC + error por stage
- [x] **Investigado S2 físico** — comando `CALIBRATE` por WS progresó (`0xB5` valores 2,3,4,5,6) y terminó `val=0`; el rojo es fallo real, no solo timeout visual.
- [ ] **Pendiente** — `slave/src/main.cpp`: acumular `calDac[4]`, `calMeas[4]`, `calTarget[4]` por stage
- [ ] **Pendiente** — `slave/src/sync_protocol.h`: definir `MsgCalResult` (44 bytes)
- [ ] **Pendiente** — `master/src/main.cpp`: relay `MsgCalResult` → JSON → WebSocket
- [ ] **Pendiente** — `master/data/js/app.js`: manejar `type:"calResult"` → `nd.calResult`
- [ ] **Pendiente** — `master/data/js/slave_panel.js`: `setCalibrationResult()` → tabla HTML
- [ ] Probar/fijar: calibrar → tabla verde/roja aparece con DAC, medido, target, error

**Dependencia:** Requiere reflash de slave ESP y master ESP.

### 7. Bug LED: no tintila cuando PSoC está IDLE
- [ ] **Pendiente investigación**:
  1. Con monitor serial del slave: confirmar si aparece `[SLAVE] BLINK_LED node=X`
  2. Con el log web: confirmar si aparece `S1 LED titilando` después de presionar botón
  3. Si el log web muestra "titilando" pero LED no prende → revisar `BLINK_LED_PIN` y nivel activo para el board específico
  4. Si log web NO muestra nada → el ACK no llegó → el slave no recibió el comando
  - **NOTA**: el guard `SAMPLING && cmd != CMD_STOP` es CORRECTO (silencio eléctrico). No tocar.
  - `ESPNOW_USE_UNICAST_TO_SLAVES=0` → master usa broadcast para todos los comandos incluyendo BLINK_LED. Esto es normal.

---

## Protocolo de versión
Los archivos JS/HTML usan query string de caché. Versión actual después de estos cambios: `field-study-3`.

## Estructura de archivos clave

```
master/data/
  index.html              — HTML principal, checkboxes de plot, tabs
  css/style.css           — Tema, responsive
  js/
    app.js                — Orquestación principal (~1530 líneas)
    plot.js               — Canvas tiempo (~840 líneas)
    spectrum.js           — Canvas FFT (~550 líneas)
    signal_proc.js        — FIR, filtFilt, dcRemove, hilbert
    data_store.js         — RingBuffer, NodeData, DataStore
    slave_panel.js        — Panel por esclavo (PGA, FIR, LED, stats)
    export.js             — ZIP/CSV export
    ws_client.js          — WebSocket con auto-reconexión
    protocol.js           — Encode de comandos WebSocket
    config.js             — Constantes (Fs, MAX_NODES, comandos)

slave/src/main.cpp        — Firmware ESP esclavo (onPsocDiag, CMD_BLINK_LED)
master/src/main.cpp       — Firmware ESP maestro (handleDirectedCmd, onDataRecv)
slave/src/sync_protocol.h — Estructuras de mensajes ESP-NOW compartidas
```

## Notas de debugging LED (agregar resultados aquí)

- Fecha:
- Estado del sistema (IDLE/ARMED):
- Log web muestra "LED titilando": SÍ / NO
- Log serial slave muestra `[SLAVE] BLINK_LED`: SÍ / NO
- LED físicamente titila: SÍ / NO
- Conclusión:
