# Codex handoff MASW web/ESP/PSoC

Fecha: 2026-06-30

Este es el documento principal para continuar el trabajo web/ESP/PSoC sin
depender de handoffs dispersos. Los documentos chicos quedan linkeados abajo
solo cuando tienen detalle tecnico util.

## Estado actual resumido

- Master ESP32: COM8, AP `GeoNetwork`, web en `http://192.168.4.1/`.
- Slave geofono fisico: COM12, node id 2, MAC `C8:2E:18:68:5F:6C`.
- PSoC fisico conectado al slave COM12: programado como GEO.
- El PSoC reporta tipo por `PSOC_HW_CLASS`; el slave lo envia en HELLO como
  `hw_class`; el master lo relaya a la web como status subtype `0x06`.
- Prueba real por WebSocket:
  - S2 reporto `type=GEO`, `psoc=True`, `fsExact=2929`.
  - ARM `n=1` y START `293` lotes dieron `8790` muestras de S2.
  - Latencia recibida: `4653 us`.
- Dummy Hammer desde PC:
  - Script: `simulate_hammer_dummy.py`.
  - Endpoint del master: `/sim/hello`.
  - Sirve para validar UI Hammer sin cambiar el PSoC GEO fisico.
- Calibracion S2:
  - Progreso `0xB5`: `2,2,3,2,2,2,4,2,2,2,2,2,5,6`.
  - Final `val=0`: fallo real de calibracion, no solo timeout visual.
- LED S2:
  - La web/log mostraba `S2 LED titilando`, pero eso solo significaba ACK.
  - El usuario no ve el LED fisico parpadear. Pendiente revisar pin real,
    nivel activo y duracion/patron del blink en firmware slave.
  - Cambio aplicado: el log web dira `LED blink ACK`; el slave usa patron mas
    largo (`BLINK_TIMES=20`, `BLINK_INTERVAL_MS=250`) y explicita
    `BLINK_LED_PIN=2`, `BLINK_LED_ACTIVE_LOW=0` en `platformio.ini`.
  - Si sigue sin verse tras flashear S2, hay que identificar el GPIO real del
    LED de esa placa y cambiar `BLINK_LED_PIN`.

## Links a documentos de detalle

- Bitacora de pruebas web/hardware:
  [`master/WEB_FIELD_TESTS.md`](master/WEB_FIELD_TESTS.md)
- Build/programacion PSoC:
  [`../psoc/BUILD_PROGRAM_PSOC.md`](../psoc/BUILD_PROGRAM_PSOC.md)
- Detalle historico de calibracion PSoC:
  [`../psoc/AcondicionamientoAnalogico.cydsn/HANDOFF_CALIBRATION.md`](../psoc/AcondicionamientoAnalogico.cydsn/HANDOFF_CALIBRATION.md)
- README master:
  [`master/README.md`](master/README.md)
- README slave:
  [`slave/README.md`](slave/README.md)

## Aclaracion: "S1 fantasma"

"S1 fantasma" era el placeholder visual que aparecia como `Hammer (S1)` aunque
el unico esclavo fisico conectado fuera S2. La UI no debe asumir que los nodos
son contiguos ni que empiezan en 1. Debe poder mostrar, por ejemplo, solo S5 si
ese es el unico esclavo que saludo.

## Pedido nuevo del usuario

1. La UI debe soportar esclavos no contiguos y de cualquier ID razonable:
   ejemplo S5 conectado aunque S1-S4 no existan.
2. `Offset Y (mV)` no debe vivir en el panel Geo/Hammer. Debe vivir en
   preservacion para desplazar capturas preservadas.
3. `Sin offset DC` debe afectar tambien a preservados.
4. En preservados, si `Sin offset DC` esta activo, se quita el DC y luego se
   suma el `Offset Y (mV)` de preservacion.
5. El valor de DC removido puede guardarse como metadata/struct al preservar,
   pero no debe quedar visualmente sumado si `Sin offset DC` esta activo.
6. La envolvente (`Env cruda`, `Env filtrada`) debe calcularse siempre sin DC,
   independientemente de los offsets visuales.
7. Preservados deben respetar seleccion de series:
   `Cruda`, `Filtrada`, `Env cruda`, `Env filtrada`.
8. Al marcar `Espectro`, debe cambiar/togglear el grafico existente, no generar
   otro grafico nuevo por cada accion.
9. `Invertir señal` debe integrarse con la politica general DC:
   - Por defecto TODO se muestra sin DC.
   - La cruda solo muestra DC si se desmarca `Sin offset DC`.
   - Preservados deben guardarse/mostrarse bajo la misma politica.
10. Orden GeoN por offset: geofonos se renombran por distancia al Hammer, pero
    los IDs fisicos S1/S2/S5 son estaticos.

## Cambios sesion 2026-06-30 (segunda pasada Claude)

### Resuelto

- **PSoC LED constante**: al corregir `TIMEOUT_COUNTS` de 24 MHz a 100 kHz
  (`1000u`), el LED parpadeaba en cada transaccion UART. Causa: dos asignaciones
  `g_comm_countdown = COMM_WINDOW_TICKS` en `uart_send_batch()` y en el handler
  de RX de `uart_service()`. Ambas eliminadas. El LED ahora permanece encendido
  (solido) durante operacion normal; solo parpadea en `wait_for_esp()` como
  indicador de busqueda del ESP. Archivo: `psoc/AcondicionamientoAnalogico.cydsn/main.c`.

- **VDAC state en panel Esclavos**: heartbeat ya guardaba `nd.vdacByte` pero no
  se mostraba. Se agrego `_lblVdac` al bloque de stats de `SlavePanel` y metodo
  publico `setVdac(vdacByte)` que muestra `VDAC: <byte> (<pct>%)`. Se llama
  desde `app.js` junto con `setPga` en el handler de heartbeat.
  Archivos: `master/data/js/slave_panel.js`, `master/data/js/app.js`.

- **Boton STATUS eliminado**: el boton no hacia nada util (solo envia CMD_STATUS
  que se hace automaticamente al conectar). Removido de `index.html` y su
  listener de `app.js`.

- **Offset X e Y en Preservados**: Codex los implemento ambos. Estan en
  `renderPreservedList()` en `app.js` (lineas ~394-436).
  Offset X: desplaza la captura en muestras (eje tiempo).
  Offset Y: desplaza verticalmente en mV despues de quitar DC.

## Cambios sesion 2026-06-30 (tercera pasada Claude)

### Resuelto

- **Nodos fantasma eliminados** (`app.js:rebalanceSlaveVisibility`):
  Se eliminó el bloque `target = Math.max(activeSlaveCount, visibleSet.size)` que
  añadía slots S1/S2/... vacíos para rellenar hasta el conteo de ARM. Ahora solo
  aparecen nodos que realmente se conectaron (tienen MAC, hwClass, o datos raw).
  Archivo: `master/data/js/app.js`.
  Ademas: hardcoded rows en `index.html` ahora empiezan con `hidden` para evitar
  flash visual antes de que el JS corra.

- **Remove DC del FIR box eliminado** (`slave_panel.js`):
  El checkbox `Remove DC` y el label `DC:` fueron quitados de la seccion FIR
  Filter del panel de esclavo. El checkbox global `Sin offset DC` en el toolbar
  de graficas es el unico control de DC. Los metodos `setDcRemove()` y
  `setDcValue()` quedan como no-ops; llamadas en `app.js` limpiadas.
  Archivo: `master/data/js/slave_panel.js`.

- **Drift eliminado del panel** (`slave_panel.js`):
  La linea `Drift: --` fue removida del bloque de estadisticas. `driftHist`
  nunca se llenaba en el flujo de captura actual, la linea solo mostraba `--`.
  La firma de `updateStats()` mantiene `_driftStr` como parametro ignorado
  para no romper llamadas existentes.
  Archivo: `master/data/js/slave_panel.js`.

- **Offset Y eliminado del panel de esclavo** (`slave_panel.js`):
  El campo `Offset Y (mV)` fue removido del panel individual de geo/hammer.
  El Y offset ahora vive SOLO en el panel de Preservados, donde desplaza
  verticalmente la captura preservada. `setYOffset()` queda como no-op.
  Archivo: `master/data/js/slave_panel.js`.

- **Layout de Preservados arreglado** (`app.js` + `style.css`):
  Los campos `Offset X` y `Offset Y mV` ahora estan dentro del div `meta`
  (bajo el titulo y sub), cada uno en su propia linea. La fila paso de grid
  5-col a 4-col. Texto del `sub` acortado (sin repetir X/Y que ya se ven en
  los inputs). Archivo: `master/data/js/app.js`, `master/data/css/style.css`.

- **Envolvente con Y offset de preservados** (`plot.js`):
  `drawEnvBilateral(env, color, xStartSamp, yOffset=0)` ahora acepta un
  `yOffset` que centra la envolvente bilateral en ese nivel Y en vez de en 0.
  Para capturas preservadas se pasa `capture.y_offset_mv / 1000` (propagado
  via `overlay.y_offset_mv` en `PlotArea.update`).
  `includeEnvelope` actualizado para usar `yOffset` en el auto-scaling.
  La envolvente de seniales en vivo siempre usa `yOffset=0` (siempre sin DC).
  Archivo: `master/data/js/plot.js`.

## Cambios sesion 2026-06-30 (cuarta pasada Claude)

### Resuelto

- **Espectro toggle (CSS fix)** (`style.css`):
  Causa raiz: `.plot-area { display: flex }` tenia mayor especificidad que el
  atributo `[hidden]` del navegador, impidiendo que `#spectra` se ocultara.
  Fix: se agrego `[hidden] { display: none !important; }` al inicio de style.css.
  Archivo: `master/data/css/style.css`. Version: `field-study-5`.

- **START bloqueado por auto-calibracion** (`slave/src/main.cpp`):
  Causa raiz: cuando auto-cal estaba programada (pero no iniciada aun,
  `!g_auto_cal_requested && g_auto_cal_due_ms != 0`), la recepcion de CMD_PRESTART
  retornaba early sin llamar `enterHotWait()`. El slave nunca entraba en HOT_WAIT
  y rechazaba CMD_START con `startAllowed=false`. VER funcionaba porque usa
  CMD_VIEW que llama `enterHotWait()` directamente sin verificar auto-cal.
  Fix: en lugar de `return`, se cancela la auto-cal pendiente (`g_auto_cal_due_ms=0`)
  y se procede a `enterHotWait()`. El usuario puede calibrar manualmente despues.
  Lineas afectadas: ~1636-1640 de `slave/src/main.cpp`.
  Slave reflasheado: `slave2` en COM12 (NODE_ID=2, geofono fisico).

### Pendiente

- **LED fisico S2**: ACK llega al web (`LED blink ACK`) pero el LED no se ve.
  Posible causa: GPIO2 no es el LED en esa placa especifica. Verificar con
  monitor serial del slave (`[SLAVE] BLINK_LED pin=2 active_low=0`) y probar
  con `BLINK_LED_PIN` diferente en `slave/platformio.ini`.

- **Todos los VDACs GEO** (4 valores: PGA, BP, ADDER, LP): El heartbeat solo
  lleva 1 byte de VDAC. Para mostrar los 4 valores se necesita un nuevo paquete
  de protocolo post-calibracion (ver §6 del plan en `.claude/plans/`).
  Nota: el mecanismo de "slave heartbeat" en la web (hbVdac via pkt.isHeartbeat)
  nunca recibe datos de esclavos porque el master solo envia sendHeartbeat(nodeId=0).
  Los VDAC de esclavos solo llegan via ACK de comandos de configuracion.

- **Nombres Geo por offset**: `reorderGeosByOffset()` ya existe y funciona
  cuando se cambia el campo `Offset m`. Pendiente verificar que el orden
  se guarda correctamente en localStorage y se restaura al reconectar.

## Archivos modificados importantes hasta ahora

- `master/src/sync_protocol.h`
- `slave/src/sync_protocol.h`
- `slave/src/main.cpp`
- `master/src/espnow_rx.h`
- `master/src/main.cpp`
- `master/src/matlab_transport.h`
- `master/src/web_server.h`
- `master/data/js/app.js`
- `master/data/js/protocol.js`
- `master/data/js/data_store.js`
- `master/data/js/slave_panel.js`
- `master/data/js/plot.js`
- `master/data/css/style.css`
- `master/data/index.html`
- `simulate_hammer_dummy.py`
