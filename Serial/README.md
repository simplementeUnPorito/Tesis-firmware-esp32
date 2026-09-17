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

`CMD_VIEW(N)`: reserva store local, arma el PSoC, entra en HOT_WAIT silencioso,
levanta `SYNC_TO_PSOC` con un pequeno retardo y guarda N lotes. Cuando el store
esta completo envia `CMD_VIEW ok=2`; el maestro recupera los lotes con
`CMD_REQ_BATCH`. Es store-and-forward, no transmision en tiempo real.

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

## Comandos USB de laboratorio

El firmware final esta silencioso por USB:

```ini
-DSLAVE_LOGS_ENABLE=0
-DDBG_HUMAN=0
-DDBG_MACHINE=0
-DSLAVE_USB_CMD_ENABLE=0
```

Para banco, compilar temporalmente con `SLAVE_USB_CMD_ENABLE=1`. Por el monitor
serial del ESP, baud 115200, quedan disponibles:

```
help
probe
status
stream 0|1
debugpsoc 0|1
pre N
sync
cap N
startnow N
clear
stop
cal
adc
blink
pga N
pgavdac N
```

`cap N` hace `pre N` y luego `sync`. `clear` libera el buffer store-and-forward
despues de una prueba USB para poder repetir capturas sin esperar un dump del
maestro. `stop` tambien libera ese buffer y baja `SYNC_TO_PSOC`.

Con `SLAVE_LAB_TOOLS_ENABLE=1` se agregan comandos de diagnostico mas
intrusivos: `diag`, `pins`, `quiet N MS`, `capwait N`, `startwait N`,
`rawcap N` y `fircap N`. Dejarlos apagados en firmware final.

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

El firmware final usa logs y comandos USB apagados. Habilitar logging para
diagnostico solo de forma temporal:
```ini
-DSLAVE_LOGS_ENABLE=1 -DDBG_HUMAN=1
```

Auto-calibración al arrancar:
```ini
-DPSOC_AUTO_CAL_ON_READY=1
-DPSOC_AUTO_CAL_DELAY_MS=500
-DPSOC_AUTO_CAL_RETRY_MS=3000
```

## Validacion 2026-07-02

- `slave2` subido a `COM12` con logs/comandos USB apagados.
- Master web `192.168.4.1/health`: `200 OK`, `littlefs=ok`.
- WebSocket `ws_capture_test.py --node 2 --batches 2 --stream 1`: 60 paquetes
  DATA, `RUNNING -> DUMPING`, una conexion.
- WebSocket `ws_capture_test.py --node 2 --batches 2 --stream 0`: 60 paquetes
  DATA, `RUNNING -> DUMPING`, una conexion.
- Antes de apagar comandos USB, `quiet 2 2000` confirmo HOT_WAIT sin bytes,
  pings ni diagnosticos periodicos durante la ventana (`winBytes=0`,
  `winPing=0`, `winDiag=0`).

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
