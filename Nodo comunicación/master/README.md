# ESP32 Maestro — Gateway geófono

Pasarela entre **MATLAB** (USB serie, datos binarios) y los **esclavos** (ESP-NOW).
El maestro **no muestrea ningún sensor**: coordina ARM/PRESTART/START/STOP, recibe
los lotes de los esclavos y los reenvía a MATLAB. El martillo es un esclavo más.

## Flujo
```
MATLAB ─USB(0x56)─> Maestro ─ESP-NOW─> Esclavos ─UART─> PSoC
```
1. `0xA2` ARM → broadcast `CMD_ARM`; esclavos responden `CMD_ARM_ACK`.
2. `0xA3` START (N 16 bits) → `CMD_PRESTART(N)` + handshake HOT_WAIT, luego
   `CMD_START` por **ESP-NOW** (el START real). Cada esclavo levanta el flanco a su PSoC.
3. Esclavos graban N lotes y el maestro hace **dump** (`CMD_REQ_BATCH`) a MATLAB.
4. `0xBD … 0xB2` = **Ver** (captura única de un nodo en vivo).

> `SYNC_OUT_PIN` (GPIO25) es **solo marcador de osciloscopio**, NO llega a los
> esclavos. Se compila a nada con `DEBUG_HARDWARE=0`.

## Protocolo con MATLAB (USB, `matlab_transport.h`)
- RX 4 bytes: `[0xAB][cmd][param][cmd^param]` (`0xA1` stream, `0xA2` arm, `0xA4` stop, `0xA5` status, `0xA7` debug, `0xB0` scope-multi).
- RX 5 bytes (N 16 bits): `[0xAB][cmd][n_lo][n_hi][cs]` (`0xA3` start, `0xAE` set-reclen).
- RX 6 bytes dirigido: `[0xAB][0xBD][node][sub][param][cs]` (`0xA6/0xA9/0xAA` cfg, `0xA7` debug ESP, `0xB2` Ver, `0xB3` debug PSoC).
- TX 6 bytes: `[0x56][node][type][b2][b1][b0]` (type 0 dato, 1 HB, 7 ACK, 0xFC latencia, 0xFD diag, 0xFE ready).

## Mensajes ESP-NOW (`sync_protocol.h`, compartido con el esclavo)
`CMD_ARM/START/STOP/PRESTART/HOTWAIT_QUERY/REQ_BATCH/SET_CONFIG/DEBUG_NODE`,
y nuevos **`CMD_VIEW (0x24)`** y **`CMD_DEBUG_PSOC (0x25)`**.

## Archivos
| Archivo | Rol |
|---|---|
| `src/main.cpp` | setup/loop, estados (IDLE…DUMPING), handlers de comandos MATLAB |
| `src/matlab_transport.h` | serie USB ↔ MATLAB (parser + envíos) |
| `src/espnow_rx.h` | recepción y reensamblado de lotes de los esclavos |
| `src/sync_protocol.h` | structs/IDs ESP-NOW (mantener en sync con el esclavo) |
| `src/debug_log.h` | logging unificado humano+máquina (ver abajo) |
| `src/master_log.h` | shim que incluye `debug_log.h` |

## Build (`platformio.ini`)
- `NUM_SLAVES`, `DEBUG_HARDWARE`/`DEBUG_HW_START_PIN` (pulso de scope), HOT_WAIT timings.
- **Logging**: `DBG_ENABLE` (0 = compila a cero), `DBG_STREAM=Serial1`,
  `DBG_LOG_RX/TX/BAUD`. El log va por **Serial1** (no por el USB de datos).
- Actualizar `SLAVE_MACS[]` con las MAC reales.

## Logging
- `Serial` (USB) = binario 0x56 hacia MATLAB. `Serial1` = log (humano + `#M,…` máquina).
- En MATLAB, abrir “Debug COM Maestro” para ver el log en el tab Log.
