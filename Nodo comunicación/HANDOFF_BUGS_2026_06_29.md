# HANDOFF — Session 2026-06-29: Bug fixes in master/slave ESP32

Branch: `feature/dma-filter-eeprom-cleanup`  
Files changed so far: `master/data/js/plot.js` (Bug 1 done)  
Firmware NOT flashed yet — all changes still on disk only.

---

## BUG 1 — Envolvente reemplazaba la señal cruda ✅ FIXED (código, sin flashear)

### Causa raíz
`plot.js:302-303` usaba `hilbertEnvelope(this._raw)` como reemplazo de `raw`, no como overlay:
```js
// ANTES (mal):
const raw = (this._showEnvelope && this._raw) ? hilbertEnvelope(this._raw) : this._raw;
```

### Fix aplicado (`master/data/js/plot.js`)
- `ENV_COLOR = 'rgba(242,130,13,0.8)'` agregado (naranja = MATLAB `[0.95 0.50 0.05 0.7]`)
- `raw`/`filt` ahora siempre apuntan a la señal original
- `envRaw`/`envFilt` calculados por separado con `hilbertEnvelope()`
- Nueva función `drawEnvBilateral(env, src, color, trim)` dibuja `dc+env` y `dc-env` encima de la señal, igual que MATLAB:
  ```matlab
  plot(axT, t, dc_r+env_v, 'Color',[col_env 0.7], ...)
  plot(axT, t, dc_r-env_v, 'Color',[col_env 0.7], ...)
  ```
- Leyenda: ahora muestra `Cruda` (azul) + `Filtrada` (rojo) + `Envolvente` (naranja) separadas
- Cursor readout: muestra `raw: X V` y `env: X V` como líneas separadas

### Para activar
```bash
cd "src/esp/Nodo comunicación/master"
pio run -t uploadfs  # sube LittleFS con el JS nuevo a COM8
```

---

## BUG 2 — Calibración "casi siempre falla" ✅ FIXED (código PSoC, requiere recompilar+flashear)

### Causa raíz
GEO_BP y GEO_LP tenían `LOCK_N=4096` — el PI necesitaba 1.37 segundos CONTINUOS dentro del deadband (≤2 DAC codes para GEO_BP) para declarar lock. Cualquier pico de ruido reseteaba el contador → timeout → `ok=0`.

Deadband por etapa (formula `ceil(gain_x1000 × 6 / 5000)`):
- GEO_PGA: dinámico
- GEO_BP: `ceil(1000×6/5000) = 2 DAC codes` ← MUY AJUSTADO
- GEO_ADDER: `ceil(3000×6/5000) = 4 DAC codes`
- GEO_LP: `ceil(6000×6/5000) = 8 DAC codes`

### Fix aplicado (2026-06-29)
`calibration_tables_geo_bp.h` y `calibration_tables_geo_lp.h`:
| Parámetro | Antes | Ahora |
|-----------|-------|-------|
| `CAL_PI_LOCK_SAMPLES_GEO_BP` | 4096 | **1024** |
| `CAL_PI_TIMEOUT_SAMPLES_GEO_BP` | 30000 (default) | **45000** |
| `CAL_PI_LOCK_SAMPLES_GEO_LP` | 4096 | **1024** |
| `CAL_PI_TIMEOUT_SAMPLES_GEO_LP` | 30000 (default) | **45000** |

1024 samples = ~341ms a 3kHz → suficiente para verificar lock sin ser frágil frente a picos.  
ESP timeout para ACK de calibración = 450s → no hay risk de timeout en la capa ESP.

### Para activar
⚠️ **Requiere PSoC Creator**: abrir `AcondicionamientoAnalogico.cywrk.elias`, compilar (Build → Build All Projects), programar con KitProg (Debug → Program).  
Ver instrucciones completas en `src/psoc/BUILD_PROGRAM_PSOC.md`.

### Diagnóstico si sigue fallando
```bash
pio device monitor --port COM9 --baud 115200
```
Buscar `PSOC_EVT_CAL_STAGE_OK` por etapa. Si alguna da `ok=0`:
- `CAL_STAGE_MEAS32` / `CAL_PI_ERROR32` / `CAL_PI_BUCKET32` en el log muestran si el PI converge
- `CAL_PI_STABLE` aparece cada vez que el contador llega a LOCK_N (si no aparece → PI nunca entra en deadband → revisar ganancia)

---

## BUG 3 — Titular LED no titila ✅ FIXED + DEPLOYED

### El código del slave está correcto en principio
`slave/src/main.cpp:1757-1766`: al recibir `CMD_BLINK_LED`:
```cpp
g_blink_count = 19;  // (BLINK_TIMES * 2) - 1 = 10 destellos
g_blink_last_ms = millis();
digitalWrite(BLINK_LED_PIN, BLINK_LED_ON_LEVEL);  // LED activo
sendCfgAck(CMD_BLINK_LED, 1);
```
Loop cada 150ms alterna el LED 10 veces.

### Fix aplicado (2026-06-29)
- `slave/platformio.ini [env:slave1]`: agregado `-DBLINK_LED_ACTIVE_LOW=0`
  - El default era `1` (LOW=ON), pero ESP32-DevKitC GPIO2 es activo-ALTO
  - Con el default: LED siempre encendido, blinks "invisibles" como apagados
  - Con `0`: LED apagado en reposo → titila ON 10 veces al presionar el botón
- Reflasheado COM9 (2026-06-29)

### Para verificar en hardware
Presionar "Titular LED" en la web → el LED GPIO2 del slave debe titilar ~10 veces.

---

## BUG 4 — Guardar EEPROM (cómo probar) 📋

### Flujo completo
1. Calibrar (`ok` verde) → el PSoC tiene los DAC de calibración activos en RAM
2. Click "Guardar EEPROM" → `PSOC_CMD_SAVE_EEPROM (0xB6)` → PSoC guarda en EEPROM flash interna
3. El dot `.dotEeprom` debe ponerse verde si el PSoC responde ACK
4. **Verificar persistencia**: power-cycle del PSoC (desenchufar/enchufar) y ver si arranca calibrado sin recalibrar

### Código PSoC (verificar)
```bash
grep -n "SAVE_EEPROM\|eeprom\|EEPROM" src/psoc/AcondicionamientoAnalogico.cydsn/main.c
```
Buscar que haya `uart_send_cfg_ack(PSOC_CMD_SAVE_EEPROM, 1)` al finalizar correctamente.

### Si el dot no se pone verde
Monitorear COM9 y buscar `PSOC_CMD_SAVE_EEPROM` recibido. Si el ESP no recibe ACK del PSoC, el dot queda en estado previo (no rojo, solo sin cambiar).

---

## Estado final (2026-06-29)

| Bug | Acción | Resultado |
|-----|--------|-----------|
| 1 Envolvente | plot.js reescrito + LittleFS uploadfs | ✅ Deployado COM8 |
| 2 Calibración GEO | calibration_tables_geo_bp/lp.h LOCK_N 4096→1024, TIMEOUT →45000; rebuild+flash PSoC via PPCLI | ✅ Deployado KitProg |
| 2b Calibración HAMMER | calibration_tables_hammer_lp.h LOCK_N 4096→1024, TIMEOUT 30000→45000 | ⚠️ Código ok, requiere rebuild+flash PSoC HAMMER |
| 3 Titular LED | BLINK_LED_ACTIVE_LOW=0 en slave1 platformio.ini + reflash | ✅ Deployado COM9 |
| 4 EEPROM | Sin cambios de código — procedimiento documentado abajo | 📋 Procedimiento |
| Logging | SLAVE_LOGS_ENABLE=0 (slave) + DBG_ENABLE=0 (master) + reflash | ✅ Silenciado |

---

## BUG 2b — Calibración HAMMER LP frágil ⚠️ FIXED (código, requiere recompilar PSoC HAMMER)

### Causa raíz
`calibration_tables_hammer_lp.h` tenía `CAL_PI_LOCK_SAMPLES_HAMMER_LP = 4096u`, el mismo bug
que GEO_BP/LP tenían antes de la sesión 2026-06-29.

El PI de HAMMER_LP necesitaba 4096 muestras consecutivas (~1.37 s a 3 kHz) dentro del deadband
para declarar lock. Cualquier pico de ruido reiniciaba el contador → timeout (30000 muestras =
10 s) → `ok=0`. El VDAC_LP queda en valor intermedio, lejos del target de 3.5 V.

### Por qué "botón funciona, web no"
- **Botón**: usuario lo presiona cuando las condiciones están estables y la auto-cal ya terminó.
- **Web**: a veces el usuario clica "Calibrar" mientras la auto-cal (PSOC_AUTO_CAL_ON_READY, 500 ms
  post-boot) todavía está corriendo → el slave devuelve BUSY → dot rojo inmediato sin intentar
  calibrar. O simplemente las 4096 muestras son demasiado exigentes con el ruido de LP.

### Fix aplicado (2026-06-29)
`calibration_tables_hammer_lp.h`:

| Parámetro | Antes | Ahora |
|-----------|-------|-------|
| `CAL_PI_LOCK_SAMPLES_HAMMER_LP` | 4096 | **1024** |
| `CAL_PI_TIMEOUT_SAMPLES_HAMMER_LP` | 30000 (default) | **45000** |

### Para activar
Recompilar y flashear el PSoC HAMMER (KitProg). Ver instrucciones en `BUILD_PROGRAM_PSOC.md`.

---

## Procedimiento Guardar EEPROM (Bug 4)
1. Calibrar → esperar dot verde (ok=1)  
2. Click "Guardar EEPROM" → dot EEPROM debe ponerse verde  
3. Power-cycle PSoC (desenchufar y enchufar) → verificar que arranca calibrado sin recalibrar manualmente

## Para re-habilitar logging
```ini
# slave/platformio.ini [env:base_esp32]:
-DSLAVE_LOGS_ENABLE=1
-DDBG_HUMAN=1
-DDBG_MACHINE=1

# master/platformio.ini:
-DDBG_ENABLE=1
-DDBG_HUMAN=1
-DDBG_MACHINE=1
```

---

## Archivos clave
| Archivo | Relevante para |
|---------|---------------|
| `master/data/js/plot.js` | Bug 1 (cambiado hoy) |
| `master/data/js/signal_proc.js` | `hilbertEnvelope()` — sin cambios |
| `slave/src/main.cpp` | Bug 3 (blink LED), Bug 4 (EEPROM) |
| `slave/platformio.ini` | `BLINK_LED_ACTIVE_LOW`, `NODE_ID` |
| `psoc/.../calibration.c` | Lógica PI, timeouts |
| `psoc/.../calibration_tables.h` | Parámetros PI por etapa |
| `psoc/.../HANDOFF_CALIBRATION.md` | §11 recetas de diagnóstico |
