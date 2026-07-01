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

## Pendiente de implementacion en este turno

- Auditar `master/data/js/app.js`, `plot.js`, `data_store.js`,
  `slave_panel.js`, `export.js` y CSS para ubicar exactamente donde se aplican
  DC, offset, inversion, envolvente y preservados.
- Cambiar el modelo de nodos de UI para no depender de `MAX_NODES=4` como techo
  visual fijo cuando llegan HELLO/status/data de IDs mayores.
- Mover `Offset Y (mV)` desde panel de esclavo a controles de preservacion.
- Aplicar `Sin offset DC` de forma consistente a vivo y preservados.
- Hacer que envolventes se calculen sobre senial sin DC.
- Hacer que preservados respeten toggles de series.
- Revisar el toggle de espectro para reutilizar/cambiar la vista existente.
- Revisar LED fisico del slave: ACK recibido pero sin parpadeo visible para el
  usuario.
- Limpiar documentos viejos/confusos y dejar este como entrada principal.

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
- `master/data/index.html`
- `simulate_hammer_dummy.py`
