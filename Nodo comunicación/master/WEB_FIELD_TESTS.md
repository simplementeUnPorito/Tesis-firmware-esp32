# UI Web — Checklist de pruebas de campo

Validación de la interfaz web del maestro ESP32. Requiere hardware real:
maestro en COM8, AP `GeoNetwork`, celular o PC conectado a `192.168.4.1`.

## Flash previo

```bash
cd "src/esp/Nodo comunicación/master"
pio run -e esp32dev          # compilar
pio run -e esp32dev -t upload    # firmware → COM8
pio run -t uploadfs              # web (LittleFS) → COM8
```

Verificar que `/health` devuelve `ok`, `littlefs=ok`.

## Scripts de smoke desde PC

Los scripts de prueba aceptan nodo destino explicito:

```bash
python "src/esp/Nodo comunicación/master/ws_cmd_test.py" --node 2 --total 35
python "src/esp/Nodo comunicación/master/ws_capture_test.py" --node 2 --batches 2 --stream 0 --total 25
python "src/esp/Nodo comunicación/master/ws_capture_test.py" --node 2 --batches 2 --stream 1 --total 25
```

`--stream 0` selecciona RAW y `--stream 1` selecciona FIR antes de enviar VER.

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
- `ws_capture_test.py --node 2 --batches 2 --stream 1 --total 25`: ACK de
  stream FIR, VER `ok=1/2`, `RUNNING -> DUMPING`, 60 paquetes DATA.
- `ws_capture_test.py --node 2 --batches 2 --stream 0 --total 25`: ACK de
  stream RAW, VER `ok=1/2`, `RUNNING -> DUMPING`, 60 paquetes DATA.

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

Validado sin hardware (servidor estático local sirviendo `data/`, sin
maestro real): el dropdown lista las 4 opciones correctamente y no hay
errores de consola. Falta re-validar con el maestro real una vez
reflasheado (ver "Pendiente de reflash" abajo) — la selección del PSoC ya
se probó por USB directo en el esclavo, ver `BUILD_PROGRAM_PSOC.md`.

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

**Pendiente de validar con hardware real** — requiere reflash del maestro
(firmware nuevo + `uploadfs`), lo que corta el WiFi del maestro y necesita
que el usuario esté presente para reconectarse después. Compilación
verificada (`pio run -e esp32dev`, sin errores); UI verificada sin errores
de consola en servidor local pero sin datos WS reales.

## Descarga sin bajar — beforeunload (2026-07-07)

Si hay una captura preservada o en el buffer en vivo cuya firma todavía no
forma parte de un ZIP exportado con éxito, cerrar/recargar la pestaña
dispara el diálogo nativo de confirmación del navegador (`beforeunload`).
Implementado en `app.js` (`hasUnexportedData()` + listener al final del
archivo) — se recalcula al vuelo comparando `preservedCaptures` contra
`exportedCaptureSignatures` (un `Set` que se llena en `onExportRequested`
tras un `downloadBlob` exitoso), no es un flag manual que se pueda
desincronizar.

**Pendiente de validar con hardware real** por el mismo motivo que arriba
(requiere reflash del maestro). Revisar manualmente: capturar algo, NO
exportar, intentar cerrar la pestaña → debe aparecer el diálogo; exportar
y volver a intentar cerrar → no debe aparecer.

## Pendiente de reflash (2026-07-07)

`link_rssi.h`, el dropdown de 4 configs de ADC y el guard de
`beforeunload` están implementados y compilan, pero **no están
desplegados en el maestro real** (COM8) todavía: reflashearlo corta el
WiFi de la interfaz y hace falta que el usuario esté presente para
reconectarse. Comando cuando esté listo:

```bash
cd "src/esp/Nodo comunicación/master"
pio run -e esp32dev -t upload    # firmware → COM8
pio run -t uploadfs              # web (LittleFS) → COM8
```
