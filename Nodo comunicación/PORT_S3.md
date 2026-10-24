# Port masterS3 / slaveS3

Se conservaron `master` y `slave` y se añadieron dos proyectos de campo:
`masterS3` y `slaveS3`. El port original partió del commit `0aa1141`; el
2026-10-24 se regeneraron desde el firmware vigente. Incluyen captura live,
streaming con ACK/retransmisión, CAPMODE, descarga rápida, marca GEONET,
marcas de ruido, trama PSoC `0x9E`, HELLO ampliado y la web actual completa.
Los fuentes y la web se mantienen con `python tools/sync_s3.py`.
No se migraron Serial, Serial_HAMMER ni MasterFoo. Los artefactos históricos
de pruebas siguen en los proyectos originales; no son evidencia del port.

## Hardware y compilación

ESP32-S3 N16R8 (16 MiB flash quad, 8 MiB PSRAM octal), Arduino-ESP32 2.0.17
con PlatformIO `espressif32@6.10.0`. Se fijan versiones de bibliotecas para
reproducir la compilación: AsyncTCP `v3.5.0` y ESPAsyncWebServer `v3.12.1`
(últimos tags al 2026-10-22, probados en este banco; `master` sigue el HEAD
de git). No bajar ESPAsyncWebServer:
- 3.6.0: un encabezado WebSocket partido en varios segmentos TCP hacía
  `LoadProhibited` en `AsyncWebSocketClient::_onData` y reiniciaba el maestro.
- 3.9.6: no reinicia, pero de forma intermitente la cola de un cliente
  conectado crecía sin vaciarse (medido q2→q8 en 5 s): llegaban los ping y
  ningún paquete binario ni ACK. Pasó 2 veces y falló en la mayoría de las
  corridas del mismo banco, incluso tras reiniciar el maestro; con 3.12.1
  pasaron las 7 corridas.

La consola usa UART0 a través del CH343, igual
que las dos placas identificadas. USB CDC nativo está desactivado.
La partición `default.csv` ocupa los primeros 4 MiB; no usa toda la flash.
LittleFS conserva offset `0x290000` y tamaño `0x160000` del original.

## Delta S3 reproducible

| Cambio S3 | Aplicación | Motivo |
|---|---|---|
| N16R8, 16 MiB DIO, `qio_opi`, PSRAM, USB CDC off | ambos | Hardware y consola CH343 |
| AsyncTCP 3.5.0 / ESPAsyncWebServer 3.12.1 | maestro | API estable con Arduino-ESP32 2.0.17 |
| `board_pins_s3.h` forzado sólo a fuentes propias | ambos | Mapa único y guarda de target |
| GPIO7 sync; 8/9 I2C; 16/17/18 reset/UART | ambos | Evitar memoria, USB y consola |
| OLED 12/11/10/5/6; botones 38–41 | esclavo | GPIO S3 libres con pull-up interno |
| SD live FSPI 14/21/13/42 | esclavo | Sustituir 14/32/13/25 del WROOM |
| `LIVE_MAX_PAGES=2048`, ring SD=512 | esclavo | Hasta 5,90 MiB de PSRAM para live |
| RX_SCAN 1/2/4/47/48 | esclavo | Sólo GPIO no reservados |
| VSPI/HSPI y diagnósticos SD parametrizados | esclavo | Host y GPIO válidos en S3 |
| Firmas ESP-NOW/WebSocket sin parche | ambos | Compilan con las versiones fijadas |

`tools/sync_s3.py` reemplaza `masterS3/src`, `masterS3/data` y
`slaveS3/src` desde los proyectos WROOM, recrea los encabezados de placa y
aplica sustituciones verificadas (falla si cambia el contexto esperado). No
toca `platformio.ini`, README, artefactos ni herramientas de prueba S3.

Desde cada proyecto, con PlatformIO instalado:

```powershell
# Dentro de masterS3:
pio run -e masterS3
pio run -e masterS3 -t upload --upload-port COM4
pio run -e masterS3 -t uploadfs --upload-port COM4
# Dentro de slaveS3:
pio run -e slaveS3
pio run -e slaveS3 -t upload --upload-port COM8
```

Los COM son los observados en este banco, no identidades permanentes.
Verificar chip/MAC con `python -m esptool --port COMx flash-id` antes de
grabar. En esta PC PlatformIO está instalado en
`C:\Users\elias\.codex\tools\esp32s3-pio\Scripts\pio.exe`.

| Rol probado | MAC STA | Puerto observado |
|---|---|---|
| masterS3 | E0:72:A1:F4:99:90 | COM4 |
| slaveS3, NODE_ID=1 | E0:72:A1:F7:01:18 | COM8 |

Los respaldos completos previos de ambas flash (16 MiB cada uno) están en
`src/firmware/esp32/.pio/backups/`, ignorados por Git. Pueden contener estado
privado del firmware anterior. No se ejecutó borrado total ni cambios de eFuses.

## Cableado del esclavo

| Señal | GPIO ESP32 original | GPIO S3 |
|---|---:|---:|
| I2C SDA, PSoC → ESP | 21 | 8 |
| I2C SCL, PSoC → ESP | 22 | 9 |
| UART RX del ESP, reservado/diagnóstico | 25 | 18 |
| UART TX del ESP → RX PSoC | 26 | 17 |
| Solicitud de reset PSoC | 19 | 16 |
| SYNC del ESP → PSoC | 27 | 7 |
| OLED SCK | 18 | 12 |
| OLED MOSI | 23 | 11 |
| OLED CS | 33 | 10 |
| OLED DC | 16 | 5 |
| OLED RESET | 17 | 6 |
| Botón arriba | 34 | 38 |
| Botón abajo | 35 | 39 |
| Botón OK | 36 | 40 |
| Botón atrás | 39 | 41 |
| Marcador opcional START | 15 | 15 |
| SD live SCK | 14 | 14 |
| SD live MISO | 32 | 21 |
| SD live MOSI | 13 | 13 |
| SD live CS | 25 | 42 |

Botones a GND con pull-up interno habilitado. Compartir GND y respetar
niveles de 3,3 V y los pull-ups del bus I2C. El pin RX del ESP no sustituye
al I2C en la configuración de campo. La SD de campo continúa en el PSoC.
El backend local de `live_capture` usa FSPI en 14/21/13/42; este cableado
nuevo requiere verificación eléctrica en el montaje.

En el maestro, el marcador `SYNC_OUT_PIN` pasa de GPIO25 a GPIO7;
GPIO15 sigue como marcador START. El log opcional por Serial1 usa RX16/TX17.
GPIO2 del original no representa el RGB integrado del S3: la identificación
vigente se reenvía al LED del PSoC. Sin PSoC, ese LED no se puede validar.

Se evitan GPIO26–37 (memoria), 19/20 (USB), 43/44 (consola) y los pines
de arranque 0/3/45/46. Referencia:
[GPIO ESP32-S3, Espressif](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html).

## Funciones conservadas y prueba sin PSoC

Se conservan el protocolo de seis bytes USB/WebSocket, ESP-NOW, descubrimiento
de nodos, ARM/PRESTART/START/STOP, captura y descarga, UI web, configuración,
enlace con PSoC y UI local. FTM/OTA del proyecto FTS no se integra en este port:
FTS se mantiene en `C:\GitHub\Tesis\src\firmware\ftm-test`.

El simulador es la rampa ESP existente, activada con el comando dirigido
`0xBD / nodo / 0xA7 / 1`. No finge que existe un PSoC ni genera medidas físicas.
Se corrigió en slaveS3 que `DEBUG=0` fuese descartado durante esta rampa;
el filtro de silencio durante la captura real conserva su comportamiento.

Desde masterS3:

```powershell
python s3_link_test.py --master COM4 --slave COM8 --output artifacts/link-directed.json
```

La prueba comprueba ACK del esclavo, muestras de nodo 1 a través de la radio,
continuidad de la rampa, parada efectiva y consola sin fallo. Requiere
`pyserial`; no requiere PSoC. No valida precisión temporal, ADC, SD del PSoC,
ni continuidad eléctrica del cableado I2C/UART. OLED y botones no estaban
conectados: su funcionamiento físico queda pendiente para el montaje.
Si el esclavo recién arrancó, la prueba reintenta el dirigido hasta ver el ACK.

### Prueba web (HTTP + WebSocket)

`masterS3/web_probe` es un cliente Wi-Fi que se graba temporalmente en la
placa del esclavo; así no hace falta cambiar la red de la PC. Se asocia a
`GeoNetwork`, baja los 15 archivos de la SPA comparando contra
`Content-Length` (incluido `app.js` de 122 KB), abre `/ws` (contesta los
ping con pong, como un navegador) y envía `{"cmd":"A1","param":0}`. Pasa si
llega el ACK `56 FF 07 A1 00 00` como trama binaria. Después prueba el lock
de un cliente desde la misma IP: un segundo `/ws` recibe
`{"type":"busy","reason":"ws_owner_alive"}` y se cierra, el dueño sigue
recibiendo ACK, y `/ws?takeover=1` cierra al dueño y pasa a recibir los ACK.

```powershell
# Dentro de masterS3 (maestro ya grabado en COM4, con uploadfs):
pio run -d web_probe -e probe -t upload --upload-port COM8
# Leer COM8 a 115200 y buscar "WEBTEST RESULT PASS"; después regrabar slaveS3.
```

Resultado 2026-10-22 con 3.12.1: `WEBTEST RESULT PASS` en todos los casos
(artifacts/web-probe-multiclient.txt). Con `-DWEBTEST_FRAGMENT_WS=1
-DWEBTEST_ONLY_STEP1` el comando sale byte a byte y el ACK llega igual
(artifacts/web-probe-fragmented.txt). No se probó con un navegador real ni
con clientes desde IP distintas (el banco tiene una sola placa libre).

Los fuentes `main_selftest.cpp` y sus herramientas se conservan como referencia,
pero están excluidos del único entorno slaveS3 de campo. No se anuncia una
tercera imagen de autotest ni resultados de los antiguos ensayos como pruebas S3.
