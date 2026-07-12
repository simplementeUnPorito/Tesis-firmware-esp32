# UI Web — Checklist de pruebas de campo

Validación de la interfaz web del maestro ESP32. Requiere hardware real:
maestro en COM8, AP `GeoNetwork`, celular o PC conectado a `192.168.4.1`.

Estado vigente del banco y veredictos: `docs/plan_pruebas_precampo.md` (matriz
fina) y `docs/HANDOFF_SESION_2026-07-12.md` (operativa y estado al cierre).
Esos dos documentos prevalecen ante cualquier nota histórica fechada de este
checklist; una prueba descrita aquí no implica que ya haya sido ejecutada.

## Flash previo

```bash
cd "src/esp/Nodo comunicación/master"
pio run -e esp32dev                                      # compilar
pio run -e esp32dev -t upload --upload-port COM8        # firmware → COM8
pio run -e esp32dev -t uploadfs --upload-port COM8      # web (LittleFS) → COM8
```

Verificar que `/health` devuelve `ok`, `littlefs=ok`.

En el banco vigente, el GEO físico de COM12 usa el firmware lógico
`slave1`/`NODE_ID=1`:

```bash
cd "src/esp/Nodo comunicación/slave"
pio run -e slave1 -t upload --upload-port COM12
```

El `upload_port` predeterminado de `slave1` en `platformio.ini` es otro; por
eso el override `--upload-port COM12` es obligatorio para este banco. Después
del reflash aplicar `ToggleReset` al PSoC y esperar la auto-calibración, según
el handoff vigente.

## Scripts de smoke desde PC

Los scripts de prueba aceptan nodo destino explicito:

```bash
python "src/esp/Nodo comunicación/master/ws_cmd_test.py" --node 1 --total 35
python "src/esp/Nodo comunicación/master/ws_capture_test.py" --self-test
python "src/esp/Nodo comunicación/master/ws_capture_test.py" --node 1 --batches 2 --fs 2604 --output smoke_geo_2b.i24le --force
```

La CLI actual de `ws_capture_test.py` no tiene opciones `--stream` ni
`--total`: es un probe GEO estricto que fuerza decimación 1 y SD habilitada,
exige HELLO fresco GEO+PSoC+SD y genera el binario más un JSON de evidencia.
Para dos lotes, el criterio esperado es 60 muestras exactas. La selección
RAW/FIR se prueba desde la UI o con un driver WS dirigido; no se debe intentar
repetirla pasando flags de la CLI antigua a este script.

### Interferencia por pestania de UI abierta (takeover del WS)

El master admite UN cliente WS. Una conexion nueva desde la MISMA IP roba el
socket (takeover, close 1001) y la UI del navegador reconecta sola para
siempre: si quedo una pestania abierta con `http://192.168.4.1/` en la misma
PC desde la que corre el script, ambos se roban el socket cada 1-2 s y el
dump de VER se aborta a mitad (`DUMPING -> ARMED`, `DATA=0` en el probe).
Sintoma tipico: `ws_capture_test.py` termina con `total connects=5` o mas y
`DATA pkts received=0` aunque los ACK (`CMD_VIEW ok=2`) lleguen bien.

Mitigaciones (2026-07-02):

1. Lo ideal: cerrar toda pestania con la UI antes de correr scripts de WS.
   `netstat -ano | findstr 192.168.4.1` delata al proceso dueno del socket.
2. `GET /ws-reset` cierra todos los clientes WS del master (la UI reconecta
   ~1 s despues, asi que solo abre una ventana corta).
3. Si no se puede cerrar la pestania, usar la estrategia del probe
   `ws_fast_probe.py` (scratchpad de la sesion 2026-07-02, documentada aqui):
   reconectar al instante tras cada robo (cada re-robo hace crecer el backoff
   de la UI: ~2.5 -> 5 -> 10 s) y RETRASAR el VER hasta despues de >=2 robos,
   para que el dump completo caiga dentro de una ventana larga. Con eso se
   midio `DATA=60/60` en RAW y FIR con una pestania hostil abierta.

## Smoke test

1. Celular conectado a `GeoNetwork` (pass `geophone2026`).
2. Abrir `http://192.168.4.1/`.
3. Confirmar que la página carga y el estado WS dice "conectado".
4. Verificar heartbeat: estado `IDLE` o `ARMED` aparece en logs.

## Path de control

1. `ARM` con el conteo de esclavos esperado.
2. Verificar READY, HELLO por esclavo: MAC, fs, tipo (GEO/HAMMER).
3. Cambiar PGA, VDAC, PGAvdac desde el panel de cada esclavo y confirmar ACK.
4. Verificar que el panel muestra `VDAC: <byte> (<pct>%)` en stats.
5. Ejecutar **Latency probe** y ver valor en µs.
6. Ejecutar **Ver** (HOT_WAIT + store-and-forward, VER mode) para cada esclavo.

## Path de captura

1. ARM → START con duración corta (p.ej. 102 lotes ≈ 1.18 s a 2604 Hz).
2. Confirmar que aparecen trazas crudas y filtradas para cada nodo.
3. Probar controles de display: `Sin offset DC`, `Espectro`, `Invertir señal`.
4. Probar envolvente: `Env cruda`, `Env filtrada`.
5. Usar cursores C1/C2 y confirmar readout en mV y ms.
6. Preservar captura → ajustar Offset X e Y → verificar desplazamiento.
7. Confirmar que los esclavos se rotulan correctamente: Hammer fija offset 0,
   GEO se ordena por distancia → Geo1/Geo2/... por `Offset m`.

## Path de calibración

1. Enviar `Calibrar` a un esclavo GEO.
2. Confirmar que el dot de calibración pasa de gris a verde si OK.
3. Verificar tabla VDAC: DAC final, mV (`code × 16`), target mV, error.
4. `Guardar EEPROM` → confirmar dot EEPROM verde.
5. Power-cycle del PSoC → reconectar → verificar que arranca con calibración
   previa (auto-cal en <500 ms, `ok=1` sin mover DACs si ya estaban en rango).

## Path de exportación

1. Tomar una captura y descargar ZIP.
2. En PC: `python src/python/geophone_scope/zip_to_mat.py capture.zip`
3. Abrir `.mat` → verificar `raw`, `filt`, `fs`, `pga_code`, `cal_vdacs`.

## Validacion 2026-07-02

- `/health`: `200 OK`, `littlefs=ok`, `ap_ip=192.168.4.1`.
- `ws_cmd_test.py --node 2 --total 35`: ACK para ARM, PGA, VDAC, PGAVDAC,
  LATENCY y STOP; HELLO/STATUS del nodo 2.
- El probe heredado de captura FIR del nodo 2 obtuvo ACK de stream, VER
  `ok=1/2`, `RUNNING -> DUMPING` y 60 paquetes DATA.
- El probe heredado de captura RAW del nodo 2 obtuvo ACK de stream, VER
  `ok=1/2`, `RUNNING -> DUMPING` y 60 paquetes DATA.

Estos dos resultados son evidencia histórica de 2026-07-02, no comandos para
la CLI actual ni pruebas nuevas de la configuración vigente.

Resultados 2026-07-02 (tarde, PSoC con politica de eventos
determinismo-primero; ver `docs/psoc_supermaquina_handoff.md`):

- Con una pestania de UI abierta en la PC, `ws_capture_test.py` fallaba con
  `DATA=0` por el takeover del WS (ver seccion de interferencia arriba).
- `ws_fast_probe.py 2 2 0 60` (nodo 2, 2 lotes, RAW): `DATA=60/60`,
  `RUNNING -> DUMPING`, ACK `CMD_VIEW ok=2`.
- `ws_fast_probe.py 2 2 1 60` (FIR): `DATA=60/60`.
- Cross-check por el log USB del esclavo (lab build): `cfg B7 ok=1`,
  `HOT_WAIT`, `START_OK sync 0->1`, `FULL -> STOPPED (2 batches)` en ambos.
- Repetido con el esclavo ya silenciado (firmware final): RAW `60/60` y
  FIR `60/60`.

## Check RF (silencio electromagnético)

Comparar espectros con/sin celular conectado:
1. Captura sin celular conectado al AP.
2. Captura con celular conectado y UI abierta.
3. Comparar: no deben aparecer picos espurios nuevos en la banda 5-50 Hz.

`beacon_pause` durante captura eleva el beacon interval a 60000 TU (~61 s)
para silencio RF. El celular puede desconectarse del AP durante la captura;
los datos quedan en el ESP y se recuperan al reconectar.

## Dummy Hammer (sin hardware)

Para validar la UI sin el PSoC Hammer físico:

```bash
python "src/esp/Nodo comunicación/simulate_hammer_dummy.py" \
  --host 192.168.4.1 --node 1 --type hammer --fs 2604 --psoc 1
```

La UI debe mostrar `Hammer (S1)` sin el campo "Offset m".

## Rango ADC — 4 configs (2026-07-07)

El dropdown "Rango ADC" de cada panel de esclavo ahora tiene 4 opciones
(antes 2): `±2.5 V`, `±0.512 V`, `±1.024 V`, `±0.625 V` — códigos 1..4,
`cfg.ADC_CONFIGS` en `config.js`. Todas a la misma Fs (2604 Hz), asi que no
hace falta re-sincronizar nada al cambiar de rango.

Evidencia registrada el 2026-07-07: el servidor estático local mostró las
cuatro opciones sin errores de consola y la selección del PSoC se probó por
USB directo en el esclavo (ver `BUILD_PROGRAM_PSOC.md`).

**Cerrado 2026-07-12**: E16 (`slave/adc_decim_transitions_test.py`) validó las
cuatro configs con transiciones dirigidas r1→r2→r4→r3→r1 a nivel
esclavo→PSoC (ACK 0xBA + mini-captura con fs correcta en cada una), y E14
validó el path navegador→maestro→esclavo→PSoC con el round-trip real
±2.5→±0.512→±2.5 desde el dropdown de la UI publicada (indicador `Current:`
actualizado + punto ACK verde en ambos sentidos). Veredictos en la matriz.

## Intensidad de señal (RSSI) — 2026-07-07

Dos indicadores nuevos:

- **Interfaz <-> maestro**: WiFi RSSI del cliente conectado al AP
  (`esp_wifi_ap_get_sta_list`), en la barra de estado (`WiFi interfaz: X
  dBm (N cliente/s)`).
- **Esclavo <-> maestro**: RSSI de ESP-NOW por nodo (sniffer promiscuo
  filtrando Action frames por MAC origen — ESP-NOW no expone RSSI en el
  callback normal de este core), en cada panel de esclavo junto al MAC.

Implementación: `master/src/link_rssi.h`. El maestro reporta por WS (JSON
`{"type":"link",...}`) cada 3 s **solo con `g_state == IDLE`**: durante
ARM/PRESTART/SAMPLING/DUMPING el sniffer promiscuo se apaga del todo (no
solo se silencia el reporte) para no competir por radio/CPU con la ventana
crítica de captura. La UI se queda con el último valor mostrado hasta
volver a IDLE.

**Cerrado 2026-07-12 (E14)**: en la UI real publicada se observaron ambos
indicadores con valores reales — `WiFi interfaz: -27 dBm (1 cliente/s)` en la
barra de estado y `RSSI: -16 dBm` junto al MAC en el panel del esclavo Geo1 —
antes, durante y después de una captura completa de 512 lotes desde la UI
(el panel conserva el último valor durante la captura, como está diseñado).
Veredicto E14 en la matriz.

## Descarga sin bajar — beforeunload (2026-07-07)

Si hay una captura preservada o en el buffer en vivo cuya firma todavía no
forma parte de un ZIP exportado con éxito, cerrar/recargar la pestaña
dispara el diálogo nativo de confirmación del navegador (`beforeunload`).
Implementado en `app.js` (`hasUnexportedData()` + listener al final del
archivo) — se recalcula al vuelo comparando `preservedCaptures` contra
`exportedCaptureSignatures` (un `Set` que se llena en `onExportRequested`
tras un `downloadBlob` exitoso), no es un flag manual que se pueda
desincronizar.

**Cerrado 2026-07-12 (E14, verificación behavioral sin modal)**: con una
captura de 512 lotes sin exportar, un `dispatchEvent(new Event('beforeunload',
{cancelable:true}))` sintético devolvió `defaultPrevented=true` (guard
activo); tras exportar el ZIP con éxito, el mismo dispatch devolvió
`defaultPrevented=false` (guard limpio). El dispatch sintético ejecuta el
listener real de `app.js` sin abrir el diálogo nativo (que bloquearía la
automatización del navegador). La única parte no automatizable — ver el
diálogo nativo en pantalla — queda como gesto manual trivial del operador,
con la lógica subyacente ya validada en ambos sentidos.

## Estado frente al cierre pre-campo (2026-07-12)

La afirmación histórica de que estas tres funciones aún esperaban un reflash
ya no describe el banco cerrado. Con la segunda tanda del 2026-07-12 las tres
comprobaciones que este checklist mantenía como manuales quedaron cerradas:
las cuatro configs ADC (E16 por USB + round-trip navegador→PSoC en E14), los
indicadores RSSI con valores reales (E14) y la lógica `beforeunload` en ambos
sentidos (E14, dispatch sintético). Veredictos y evidencia en
`docs/plan_pruebas_precampo.md`.

Si una prueba futura vuelve a cargar firmware o web, usar puertos explícitos:

```bash
cd "src/esp/Nodo comunicación/master"
pio run -e esp32dev -t upload --upload-port COM8
pio run -e esp32dev -t uploadfs --upload-port COM8
```

Luego recuperar `GeoNetwork` con
`python reconnect_geonetwork.py --timeout 120` (WlanConnect por ctypes) y
verificar `/health`; la receta completa está en el handoff.
