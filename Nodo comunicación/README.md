# Sistema ESP32 MASW — Nodo comunicación

Documentación del subsistema de comunicación inalámbrica para el sistema MASW
(Multichannel Analysis of Surface Waves). Contiene el firmware del **maestro**
y de los **esclavos**, la interfaz web embebida, y scripts auxiliares.

## Arquitectura general

```
PC/MATLAB ──USB 921600──> ESP Maestro (COM8, AP "GeoNetwork", 192.168.4.1)
Celular/PC ──WiFi──────> |      ↕ ESP-NOW broadcast (canal 1)
                         |
                 ┌───────┴────────────────────────┐
                 ↓                                ↓
        ESP Slave 1 (HAMMER, COM10)   ESP Slave 2 (GEO, COM12, C8:2E:18:68:5F:6C)
                 ↓ UART 115200                    ↓ UART 115200
           PSoC HAMMER                      PSoC GEO
```

El maestro **no muestrea ningún sensor**: coordina ARM/PRESTART/START/STOP y
reenvía lotes de los esclavos a MATLAB y a la UI web. El martillo es un esclavo
normal con distinto PSoC.

## Hardware activo (2026-07-01)

| Dispositivo | Puerto | NODE_ID | MAC | Tipo |
|-------------|--------|---------|-----|------|
| ESP Maestro | COM8 | — | — | AP `GeoNetwork`, pass `geophone2026` |
| ESP Esclavo 1 | COM10 | 1 | `C8:2E:18:67:1A:08` | HAMMER |
| ESP Esclavo 2 | COM12 | 2 | `C8:2E:18:68:5F:6C` | GEO (PSoC físico) |
| ESP Esclavo 3 | COM11 | 3 | — | GEO (reservado) |

## Flujo de sincronización

```
MATLAB/Web → ARM (A2)   → maestro broadcast CMD_ARM → esclavos responden ACK
MATLAB/Web → START (A3) → maestro envía PRESTART a c/esclavo
                          → esclavos arman PSoC (0xA3 setN + 0xB1 arm)
                          → maestro verifica HOT_WAIT (todos listos)
                          → maestro envía CMD_START (ESP-NOW broadcast)
                          → cada esclavo levanta SYNC_TO_PSOC (GPIO23 → PSoC SYNC_IN)
                          → PSoC arranca captura por flanco GPIO
MATLAB/Web → STOP (A4)  → maestro broadcast CMD_STOP
```

El pin `SYNC_OUT` del maestro (GPIO15) es **solo marcador de osciloscopio** y
**no** llega a los esclavos. El arranque real es por ESP-NOW → GPIO23 del esclavo.

## Protocolo binario (maestro ↔ MATLAB, mirrored to WebSocket)

### TX maestro → MATLAB/WS: paquetes de 6 bytes

`[0x56][node_id][type][b2][b1][b0]`

| type | Significado |
|------|-------------|
| `0x00` | Muestra ADC (int24 signed, b2:b1:b0) |
| `0x01` | Heartbeat (b0 = MasterState, b1 = PGA, b2 = VDAC) |
| `0x07` | ACK (b2 = cmd, b1:b0 = valor) |
| `0xFC` | Latencia START (µs, 24-bit unsigned) |
| `0xFD` | Status/HELLO (bloque multi-frame) |
| `0xFE` | READY (b0 = n_slaves_ready) |

### RX MATLAB/WS → maestro: comandos

| Bytes | Formato | Comando |
|-------|---------|---------|
| 4 | `[0xAB][cmd][param][cs]` | `A1` stream, `A4` stop, `A5` status, `A7` debug, `B0` scope-multi |
| 5 | `[0xAB][cmd][n_lo][n_hi][cs]` | `A2` ARM (n esclavos), `A3` START (N lotes), `AE` set-reclen |
| 6 | `[0xAB][0xBD][node][sub][param][cs]` | `A6` PGA, `A9` PGAvdac, `AA` VDAC, `B2` Ver, `B5` calibrar, `B6` EEPROM, `B7` stream, `B9` blink LED |

Equivalentes JSON para WebSocket:
```json
{"cmd":"A2","param":1}        // ARM 1 esclavo
{"cmd":"A3","value":293}      // START 293 lotes
{"cmd":"BD","node":2,"sub":"B5","param":1}  // calibrar esclavo 2
```

## Interfaz web (master/data/)

SPA servida desde LittleFS del maestro. Versión actual del cache: `field-study-10`.

### Endpoints HTTP

| Endpoint | Descripción |
|----------|-------------|
| `/` | UI principal (index.html) |
| `/health` | Estado: `ok`, `ap_ip`, `littlefs`, `used/total bytes` |
| `/ws` | WebSocket — telemetría binaria + comandos JSON |
| `/ws-reset` | Fuerza cierre de conexiones WS activas |
| `/sim/hello` | Dummy esclavo: `?node=N&type=hammer|geo&fs=2929&psoc=1` |

### Módulos JS

| Archivo | Rol |
|---------|-----|
| `app.js` | Orquestación principal (~1600 líneas) |
| `plot.js` | Canvas tiempo: cruda, filtrada, envolvente bilateral |
| `spectrum.js` | Canvas FFT |
| `signal_proc.js` | `filtFilt`, `dcRemove`, envolvente Hilbert |
| `data_store.js` | `RingBuffer`, `NodeData`, `DataStore` |
| `slave_panel.js` | Panel por esclavo: PGA, FIR, LED, stats, calibración |
| `export.js` | ZIP con CSVs, JSON y binario f32le por canal |
| `ws_client.js` | WebSocket con auto-reconexión y auth opcional |
| `protocol.js` | Encode de comandos |
| `config.js` | Constantes: Fs, MAX_NODES, códigos de comando |
| `zip_store.js` | Acumulación de capturas para exportar |

### Funcionalidades activas

- **Detección automática de tipo**: HELLO desde firmware define si es GEO o HAMMER.
  Hammer fuerza offset 0 y oculta "Offset m"; GEOs se ordenan por distancia (Geo1, Geo2...).
- **Nodos no contiguos**: la UI muestra solo nodos que realmente se conectaron.
- **Captura MASW**: ARM N → START → dump de N lotes por nodo.
- **Visualización**: cruda, filtrada (`filtFilt`), `Env cruda`, `Env filtrada` (Hilbert bilateral).
- **Sin offset DC**: checkbox global quita el DC a cruda y filtrada.
- **Espectro**: toggle — reemplaza el plot de tiempo con FFT.
- **Invertir señal**: por canal.
- **Preservados**: capturas guardadas con Offset X (muestras) e Y (mV) ajustables.
- **VDAC en stats**: heartbeat muestra `VDAC: <byte> (<pct>%)` en el panel de cada esclavo.
- **Calibración VDAC**: tabla con DAC final, mV, target, error mV y error % por etapa.
- **Export ZIP**: `node/raw_f32le.bin`, `node/filt_f32le.bin`, CSVs, JSON de metadatos.
- **Dummy Hammer**: `simulate_hammer_dummy.py` + endpoint `/sim/hello` para validar UI.

### Nota: beacon_pause y WiFi

Durante captura/dump, el maestro pausa el beacon AP a 60000 TU (~61 s) para
silencio RF. La UI detecta la caída y reconecta automáticamente. En capturas
largas (>30 s) el PC puede desconectarse del AP — **los datos quedan en el ESP
y se pueden recuperar al reconectar**; la adquisición no se interrumpe.

## Build y flash

### Maestro (desde `master/`)

```bash
pio run -e esp32dev          # compilar
pio run -e esp32dev -t upload    # flashear firmware (COM8)
pio run -t uploadfs              # subir LittleFS con la web (COM8)
```

### Esclavos (desde `slave/`)

```bash
pio run -e slave1            # compilar HAMMER (NODE_ID=1)
pio run -e slave1 -t upload  # flashear esclavo 1 (COM10)
pio run -e slave2 -t upload  # flashear esclavo 2 (COM12)
```

Tamaños de build referencia (2026-07-01):

| Firmware | Flash | RAM |
|----------|-------|-----|
| Maestro (esp32dev) | 68.1% | 15.4% |
| Esclavo 2 (slave2) | 57.0% | 13.5% |

## Archivos fuente

```
master/
  src/main.cpp              — firmware maestro: estados, ARM/START/STOP, dump
  src/web_server.h          — HTTP + LittleFS + endpoint /sim/hello
  src/web_relay.h           — espejo WS de paquetes binarios, decode JSON→cmd
  src/matlab_transport.h    — protocolo USB ↔ MATLAB (parser + TX)
  src/espnow_rx.h           — recepción y reensamblado de lotes ESP-NOW
  src/sync_protocol.h       — structs ESP-NOW (compartido con esclavos)
  data/                     — SPA web (index.html, css/, js/)

slave/
  src/main.cpp              — firmware esclavo: estados, ESP-NOW, store-forward
  src/psoc_uart.h/.cpp      — enlace UART con PSoC (parser + comandos)
  src/espnow_transport.h    — envío de lotes al maestro
  src/sync_protocol.h       — structs ESP-NOW (mismos que maestro)

simulate_hammer_dummy.py    — simula un HELLO de Hammer sin PSoC real
```
