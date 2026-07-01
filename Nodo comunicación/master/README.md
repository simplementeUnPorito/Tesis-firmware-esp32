# ESP32 Maestro — Gateway geófono

Pasarela entre **MATLAB/Web** y los **esclavos** (ESP-NOW). No muestrea ningún
sensor; coordina ARM/PRESTART/START/STOP, recibe lotes de los esclavos y los
reenvía a MATLAB. La UI web es una SPA embebida servida desde LittleFS.

## Flujo de datos

```
MATLAB ──USB 921600──> Maestro ──ESP-NOW──> Esclavos ──UART──> PSoC
Web/Celular ──WiFi──>  |      <───lotes──────────────────────────
                       └──LittleFS──> SPA (192.168.4.1)
```

1. `A2` ARM → broadcast `CMD_ARM`; esclavos responden `CMD_ARM_ACK`.
2. `A3` START (N lotes) → `CMD_PRESTART(N)` + handshake HOT_WAIT, luego
   `CMD_START` por **ESP-NOW broadcast**. Cada esclavo levanta SYNC_TO_PSOC.
3. Esclavos capturan N lotes y el maestro hace dump (`CMD_REQ_BATCH`).
4. `BD … B2` = **Ver** (captura en vivo de un solo nodo).

`SYNC_OUT_PIN` (GPIO15) es **solo marcador de osciloscopio**; NO llega a los
esclavos ni dispara ningún PSoC.

## Máquina de estados

`IDLE → ARMING → ARMED → PRESTART → RUNNING → STOPPING → DUMPING`

Modo adicional: `SCOPE_MULTI` (múltiples starts para caracterizar jitter).

## Protocolo con MATLAB/Web (6 bytes TX)

`[0x56][node][type][b2][b1][b0]`

| type | Significado |
|------|-------------|
| `0x00` | Muestra ADC raw |
| `0x01` | Heartbeat (estado maestro, PGA, VDAC) |
| `0x07` | ACK (b2=cmd, b1:b0=valor) |
| `0xFC` | Latencia START µs |
| `0xFD` | Status/HELLO multiframe |
| `0xFE` | READY (n_slaves) |

## Comandos recibidos

| Bytes | Cmd | Descripción |
|-------|-----|-------------|
| 4 | `0xA4` | STOP |
| 4 | `0xA5` | STATUS |
| 4 | `0xA7` | DEBUG |
| 4 | `0xB0` | SCOPE_MULTI |
| 5 | `0xA2` | ARM (n esclavos) |
| 5 | `0xA3` | START (N lotes, 16 bits) |
| 5 | `0xAE` | SET_RECLEN |
| 6 | `0xBD … 0xA6` | PGA por esclavo |
| 6 | `0xBD … 0xA9` | PGAvdac por esclavo |
| 6 | `0xBD … 0xAA` | VDAC por esclavo |
| 6 | `0xBD … 0xB2` | Ver (captura en vivo) |
| 6 | `0xBD … 0xB5` | Calibrar PSoC |
| 6 | `0xBD … 0xB6` | Guardar EEPROM |
| 6 | `0xBD … 0xB7` | Seleccionar stream crudo/FIR |
| 6 | `0xBD … 0xB9` | Titilar LED del PSoC |
| 6 | `0xBD … 0xAF` | Latency probe |

Equivalentes JSON para WebSocket: `{"cmd":"A2","param":1}`,
`{"cmd":"BD","node":2,"sub":"B5","param":1}`, etc.

## Interfaz web

Servida desde LittleFS en `data/` (SPA versión `field-study-10`).

- **WebSocket** `/ws`: paquetes binarios hacia cliente + comandos JSON desde cliente.
- **`/health`**: estado del servidor y LittleFS.
- **`/sim/hello`**: dummy esclavo para validar UI sin hardware real.
- **Auth opcional**: `WS_AUTH_TOKEN` en platformio.ini exige password por WS.

## Archivos

| Archivo | Rol |
|---------|-----|
| `src/main.cpp` | setup/loop, estados, handlers MATLAB/WS, beacon_pause |
| `src/web_server.h` | HTTP/LittleFS, rutas, endpoint /sim/hello |
| `src/web_relay.h` | Espejo WS: binario→cliente, JSON→cmd, `webRelayCloseAll()` |
| `src/matlab_transport.h` | Serie USB ↔ MATLAB (parser + TX) |
| `src/espnow_rx.h` | Recepción y reensamblado de lotes ESP-NOW |
| `src/sync_protocol.h` | Structs/IDs ESP-NOW (mantener sync con esclavo) |
| `data/index.html` | UI principal |
| `data/js/app.js` | Orquestación JS (~1600 líneas) |
| `data/js/plot.js` | Canvas tiempo |
| `data/js/signal_proc.js` | FIR, filtFilt, dcRemove, Hilbert |
| `data/js/slave_panel.js` | Panel por esclavo |
| `data/js/export.js` | ZIP/CSV export |

## Build (`platformio.ini`)

```ini
[env:esp32dev]
upload_port = COM8
NUM_SLAVES  = 8
DEBUG_HARDWARE = 1       ; pulso de osciloscopio en GPIO15 al emitir START
DBG_ENABLE = 0           ; logging por Serial1 (0 = silenciado en campo)
```

Logging cuando se necesite diagnóstico:
```ini
-DDBG_ENABLE=1 -DDBG_HUMAN=1 -DDBG_MACHINE=1
```

## Notas de campo

- **Límite `PSOC_CAPTURE_MAX_BATCHES = 512`**: `SET_RECLEN` se silencia
  por encima de 512. Si el master queda en `DUMPING`, enviar STOP + `SET_RECLEN=0`.
- **beacon_pause**: durante captura el beacon AP sube a 60000 TU (~61 s).
  El PC puede desconectarse del AP pero los datos están seguros en el ESP.
- **No abrir COM8 con pySerial en Windows**: el DTR/RTS del CP210x hace reset
  al ESP32. Usar solo diagnóstico por WebSocket.
