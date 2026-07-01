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
6. Ejecutar **Ver** (captura en vivo, VER mode) para cada esclavo.

## Path de captura

1. ARM → START con duración corta (p.ej. 293 lotes ≈ 3 s a 2929 Hz).
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
  --host 192.168.4.1 --node 1 --type hammer --fs 2929 --psoc 1
```

La UI debe mostrar `Hammer (S1)` sin el campo "Offset m".
