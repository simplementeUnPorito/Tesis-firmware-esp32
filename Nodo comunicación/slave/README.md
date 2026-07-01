# ESP32 Esclavo — Nodo geófono/martillo

Lee muestras del PSoC por **UART** y las envía al maestro por **ESP-NOW**.
Un esclavo puede ser geófono (GEO) o martillo (HAMMER) — mismo firmware,
distinto PSoC y `PSOC_HW_CLASS` detectado por el PSoC al arrancar.

## Estados

```
WAIT_ARM → ARMED → HOT_WAIT → SAMPLING → STOPPED
```

- **WAIT_ARM**: espera `CMD_ARM` del maestro (broadcast).
- **ARMED**: envía `CMD_ARM_ACK`, espera `CMD_PRESTART`.
- **HOT_WAIT**: `CMD_PRESTART(N)` recibido → reserva buffer, arma PSoC
  (`setN` + `preStart`). Solo espera `CMD_START` o queries de maestro.
- **SAMPLING**: recibe `CMD_START` → levanta `SYNC_TO_PSOC` (GPIO23) → PSoC
  arranca. Acumula N lotes del PSoC y espera dump (`CMD_REQ_BATCH`).
- **STOPPED**: vuelve a WAIT_ARM al recibir `CMD_STOP`.

### Modo VER

`CMD_VIEW(N)`: arma el PSoC, levanta SYNC_TO_PSOC con un pequeño retardo, y
transmite N lotes **en tiempo real** al maestro (sin store-and-forward).

## Enlace con el PSoC (`psoc_uart.*`)

- **Serial2** (ESP32): `PSOC_UART_RX=17`, `PSOC_UART_TX=16`, baud=115200.
- Wiring: ESP GPIO17 → PSoC P1[2] (RX); ESP GPIO16 ← PSoC P1[5] (TX).
- **Arranque siempre por pin**: GPIO23 (`SYNC_TO_PSOC`) → PSoC `SYNC_IN` (P12[6]).
  El PSoC nunca arranca por comando UART.

## Auto-calibración

Al detectar el PSoC al arrancar, el esclavo programa una auto-calibración
500 ms después (`PSOC_AUTO_CAL_ON_READY=1`, `PSOC_AUTO_CAL_DELAY_MS=500`).
Si el maestro envía `CMD_PRESTART` mientras hay auto-cal pendiente, se cancela
y el esclavo entra en HOT_WAIT normalmente.

## Tipo de hardware (GEO/HAMMER)

El PSoC reporta `PSOC_HW_CLASS` en `PSOC_EVT_BOOT`. El esclavo lo reenvía en
`MsgHello.hw_class`. El maestro lo relaya a la web como status subtype `0x06`:
`0 = GEO`, `1 = HAMMER`, `0xFF = desconocido`.

## LED de identificación

`CMD_BLINK_LED` → esclavo envía `PSOC_CMD_BLINK_LED (0xB9)` al PSoC por UART.
El PSoC titila ~8 s a 2.5 Hz (no bloqueante). El LED físico es el del PSoC.

## Archivos

| Archivo | Rol |
|---------|-----|
| `src/main.cpp` | Firmware completo: estados, handlers ESP-NOW, store-forward, VER |
| `src/psoc_uart.h/.cpp` | Enlace UART con PSoC: parser de frames + envío de cmds |
| `src/espnow_transport.h` | Envío de lotes al maestro (fragmenta en 2× MsgData) |
| `src/sync_protocol.h` | Structs/IDs ESP-NOW (mismo que maestro) |
| `src/debug_log.h` | Logging humano + máquina (gateado por `DBG_ENABLE`) |

## Build (`platformio.ini`)

```ini
[env:slave2]  ; GEO físico
upload_port = COM12
NODE_ID     = 2
BLINK_LED_PIN = 2
BLINK_LED_ACTIVE_LOW = 0   ; GPIO2 en ESP32-DevKitC es activo-ALTO
```

Habilitar logging para diagnóstico:
```ini
-DSLAVE_LOGS_ENABLE=1 -DDBG_HUMAN=1
```

Auto-calibración al arrancar:
```ini
-DPSOC_AUTO_CAL_ON_READY=1
-DPSOC_AUTO_CAL_DELAY_MS=500
-DPSOC_AUTO_CAL_RETRY_MS=3000
```

## Logging típico de boot normal

```
[SLAVE 2] boot
MAC: C8:2E:18:68:5F:6C  ch=1
[ESPNOW] ready ch=1
[SLAVE 2] Buscando PSoC...
[PSoC] boot hw=0/GEO pstate=0/IDLE
[SLAVE 2] PSoC: DETECTADO
[AUTO_CAL] scheduled in 500 ms
[SLAVE 2] listo, esperando ARM
[AUTO_CAL] -> requesting calibration
[CAL] done ok=1
```
