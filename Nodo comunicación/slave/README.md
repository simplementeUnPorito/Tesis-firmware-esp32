# ESP32 Esclavo — Nodo geófono

Lee las muestras **raw** del PSoC por **UART**, las acumula y las envía al maestro
por **ESP-NOW**. Reenvía al PSoC la configuración (N, VDAC, PGA) y dispara el
muestreo levantando el pin `SYNC_TO_PSOC`. Un esclavo puede ser un geófono o el
martillo (mismo firmware; cambia solo el hardware del PSoC).

## Enlace con el PSoC (`psoc_uart.*`)
- **Serial2** (ESP32): `PSOC_UART_RX=16`, `PSOC_UART_TX=17`, `PSOC_UART_BAUD=460800`.
- Recibe frames de 95 B (raw 24-bit → `PsocBatch`); envía comandos
  `setN / preStart / setVdac / setPga / setPgavdac / debugRamp`.
- El **arranque del PSoC es siempre por el pin** `SYNC_TO_PSOC` (flanco), no por UART.

## Estados
`WAIT_ARM → ARMED → HOT_WAIT → SAMPLING → STOPPED`
- `CMD_PRESTART(N)`: `enterHotWait` reserva buffer y arma el PSoC (`setN`+`preStart`).
- `CMD_START`: levanta `SYNC_TO_PSOC` → el PSoC arranca; el esclavo acumula N lotes
  (store-and-forward) y queda esperando el dump (`CMD_REQ_BATCH`).
- `CMD_VIEW(N)` (**Ver**): arma el PSoC, levanta el flanco (con pequeño retardo) y
  transmite los N lotes **en vivo** al maestro (disparo único, sin store).
- `CMD_DEBUG_PSOC`: rampa de debug en el PSoC.

## Sincronización
El flanco del maestro (`SYNC_OUT`) **no** está cableado a los esclavos; el START
llega por **ESP-NOW** y cada esclavo levanta su propio pin hacia el PSoC.

## Archivos
| Archivo | Rol |
|---|---|
| `src/main.cpp` | setup/loop, estados, handlers ESP-NOW, store-and-forward, Ver |
| `src/psoc_uart.h/.cpp` | enlace UART con el PSoC (parser + comandos) |
| `src/espnow_transport.h` | envío de lotes al maestro (fragmenta en 2× MsgData) |
| `src/espnow_compat.h` | compatibilidad ESP-NOW ESP32/ESP8266 |
| `src/sync_protocol.h` | structs/IDs ESP-NOW (igual que el maestro) |
| `src/debug_log.h` | logging humano + máquina |

## Build (`platformio.ini`)
- Un `env` por nodo (`slave1/2/3`) con `-DNODE_ID=n`.
- **Logging**: `DBG_ENABLE` (0 = compila a cero). El log va por el **USB** del esclavo
  (115200); en MATLAB se ve con “Debug COM Esclavo”.
- UART al PSoC: `PSOC_UART_BAUD/RX/TX`. Actualizar `MASTER_MAC[]`.

## Logging
Líneas humanas `[t_us][Sn] …` → tab Log; líneas máquina `#M,t_us,Sn,evt,…` → log máquina.
Se registran: ESP-NOW recibidos, comandos al PSoC, lotes, cambios de estado, flancos.
