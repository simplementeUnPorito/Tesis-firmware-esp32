# Compatibilidad del maestro con placas SERIAL e I2C (2026-09-17)

## Qué placa es cuál

Cada esclavo declara dos datos independientes en el HELLO:

| Campo | Valores | Quién lo decide |
|---|---|---|
| `hw_class` | 0 = GEO, 1 = HAMMER | el PSoC, según su TopDesign |
| `link_model` | 0 = SERIAL, 1 = I2C, 0xFF = firmware viejo | el ESP, en tiempo de compilación |

`link_model` es el último byte del HELLO. Un maestro viejo lo ignora, y un
maestro nuevo acepta HELLO cortos: con 5 bytes quedan GEO/HAMMER y el enlace
en UNKNOWN.

| Placa | Firmware ESP | Firmware PSoC |
|---|---|---|
| GEO I2C | `Nodo comunicación/slave` env `slave2` (nodo 2) | `psoc/AcondicionamientoAnalogico.cydsn` |
| GEO SERIAL | `Serial` env `slave3` (nodo 3) | `psoc/Serial_HAMMER` con el TopDesign GEO |
| HAMMER SERIAL | `Serial` env `slave1` (nodo 1) | `psoc/Serial_HAMMER` |

`Serial` es la pareja del 21-jul (ESP `adf41af1`, PSoC `806a5809`), con
`link_model=SERIAL` y los arreglos de la tabla de más abajo.
`Serial_HAMMER` en esp32 es el snapshot intacto.
Del lado del ESP los pines son iguales en GEO y HAMMER seriales
(TX GPIO16, RX GPIO17, SYNC GPIO23). Lo que cambia es el pin del PSoC: el
SYNC_IN va en P12[2] en el HAMMER y en P12[6] en el GEO.

## Middleware en el maestro (`master/src/main.cpp`, `handleDirectedCmd`)

Un nodo se trata como SERIAL si su HELLO declara SERIAL, o si no declara
enlace y es HAMMER.

- PGAout (0xA8): solo I2C. Si el nodo es SERIAL, el maestro devuelve NACK sin transmitir.
- 0xAA en I2C: IDAC firmado por etapa, `p1 = signo|etapa`, `p2 = magnitud`.
- 0xAA en SERIAL: VDAC global. Por USB va en 7 bytes con `p1 = 0x40` y
  `p2 = código`; por la web va sin `param2`. Nunca se reenvía un IDAC a un
  PSoC serial, porque interpretaría la etapa como código VDAC.
- La web muestra PGAout y el IDAC firmado solo en GEO I2C, y la fila
  "VDAC global" solo en nodos SERIAL (`nodeIsSerialAnalog`, mismo criterio que
  el maestro). El "enviar config" ya no manda PGAout a nodos SERIAL.

La auditoría por lectura de código no encontró otras diferencias de protocolo
ESP-NOW entre el maestro actual y el esclavo serial: ARM, PRESTART, HOT_WAIT,
START, dump, calibración 0xB5 y sus ACK por etapa son iguales. `MsgSetConfig`
creció a 5 bytes, pero el esclavo serial valida con `>=` y no le afecta.

## Arreglos portados a la línea serial

Son arreglos posteriores al 21-jul que no dependen del I2C ni de los IDAC:

| Origen | Qué arregla | Dónde |
|---|---|---|
| `bb6b753` (24-jul) | el esclavo adopta el canal del AP del maestro. Con el canal 1 fijo quedaba sordo cuando el maestro se asocia a la red del cliente | `Serial/src/main.cpp` |
| `83613f0` (4-sep) | reintento de SETN; sin él quedaba ~4 % de capturas vacías | `Serial/src/main.cpp` |
| `443a4ca` + `8a0db0a` (1-sep) | watchdog de ARMED de 60 s en el PSoC; el ESP vuelve a STOPPED y manda NACK | `psoc/Serial_HAMMER` + `Serial` |

No se portó el `HOTWAIT_READY_TIMEOUT_MS` de 2000 ms: en el serial quedan los
700 ms con los que se validó en julio. Si aparecen `HOTWAIT_REARM` de más,
subirlo.

Tampoco se portó nada del calibrador PI, el IDAC, el autotest ni la telemetría
`ctl`: son de la electrónica I2C.

## Estado

- Verificado en placa (17-sep): el maestro en COM6 recibe del nodo 2
  `psoc=1, fs=2604, GEO, sd=1, I2C`.
- Solo compilado, sin grabar: maestro y web `field-study-21` (firmware y
  `buildfs`), esclavo I2C `slave2`, esclavo serial `slave1`/`slave2` y PSoC
  `Serial_HAMMER` (Release).
- COM6 se reenumera o queda trabado cuando se usa el puerto. La sospecha es la
  alimentación del maestro.

## Orden de prueba

1. Maestro: `pio run -t upload` y `pio run -t uploadfs` en `Nodo comunicación/master`.
2. `python probar_maestro.py COM6 hello` con la GEO I2C, y después `captura 4`.
3. HAMMER SERIAL: grabar `psoc/Serial_HAMMER` y `esp32/Serial` env `slave1`.
   Después `hello`, que debe dar HAMMER · SERIAL, y `captura 4`. Desde la web
   probar PGA, calibrar y la fila VDAC.
4. GEO SERIAL: `esp32/Serial` env `slave3`, porque el nodo 2 ya es la GEO I2C y el
   maestro admite 3 esclavos (`NUM_SLAVES`). Su PSoC necesita el TopDesign GEO
   con SYNC_IN en P12[6], que no está en este snapshot.

## Espigas cada ~57 ms en la captura (17-sep, tarde)

Síntoma: en una captura de la GEO I2C aparecían espigas periódicas, unas 20
por segundo.

Causa probable, por código y por período: con la SD habilitada el PSoC
captura siempre a SD, aunque la captura entre en RAM. El drenaje del ring
escribe un sector de 512 B cada 5 lotes × 30 muestras = 150 muestras, es decir
57,6 ms a 2604 Hz, **mientras muestrea**. Cada `f_write` es un pico de
corriente. Además hace `f_sync` cada 32 bloques (~1,8 s). El PSoC en sí no
transmite nada durante ARMED/SAMPLING, y la OLED del esclavo no dibuja en esa
ventana.

Prueba sin grabar nada: destildar SD en la web y repetir la captura. Si las
espigas desaparecen, se confirma la causa.

Arreglo, aplicado a las dos líneas:

- Regla: la SD solo se usa si la captura no entra en RAM (`N > 512` lotes
  decimados). `psoc_arm()` y `psoc_enter_sampling()` en los dos PSoC,
  `allocStore()` en los dos ESP.
- El ESP separa la preferencia del usuario (`g_sd_user_enabled`) del estado
  real del PSoC (`g_psoc_sd_capture_enabled`). Antes de cada captura
  `syncPsocSdForCapture()` le apaga o prende la SD al PSoC según haga falta.
  Por eso el arreglo funciona **sin regrabar el PSoC**: alcanza con el ESP
  nuevo. El PSoC no persiste ese flag.

## Web: panel de la placa I2C

- "Calibración" pasa a llamarse "Control" y no tiene botón. El punto se pone
  verde en banda, rojo fuera de banda y gris sin telemetría; el tooltip muestra
  el estado del lazo y del perfil.
- El dato viaja en un byte nuevo al final del HELLO (`ctl_status`: bit0 banda,
  bits1-3 estado 0..4, bit4 perfil). El maestro lo reenvía como HELLO sub
  0x09. Las placas SERIAL no lo mandan y conservan su botón Calibrar.
- Se quitó el IDAC manual de la web; el maestro lo sigue aceptando por USB.
- "Guardar EEPROM" se oculta en I2C, porque el perfil se guarda solo al
  terminar de aprender. En SERIAL sigue: el PSoC serial solo guarda los VDAC
  con 0xB6.
- Web `field-study-22`. Las capturas exportadas incluyen `ctl_status`.

## PSoC I2C principal: no compila hoy

`AcondicionamientoAnalogico.cydsn` falla por el TopDesign, no por el código:
el build detecta AnalogHammer y no encuentra `VDAC_PGA`, `VDAC_LP` ni
`AMux_ADC` (`calibration_tables.h:397`, `calibration.c:51`). El auto-guardado
del 17-sep trae componentes nuevos (`AMux_Signal`, `AMux_Vref`, `OPA_*`). El
cambio de SD en su `main.c` queda escrito pero sin compilar. No hace falta para
el arreglo de las espigas, que ya lo resuelve el ESP.

## Encadenado de tramos sin parar el ADC (17-sep)

superMaquina cuenta como máximo 512 lotes crudos por corrida (contador de 9
bits). Antes, el lazo principal encadenaba el tramo siguiente con
`ADC_StopConvert`, reconfiguración, un diag `CHAIN_NEXT` por I2C y
`ADC_StartConvert`: quedaba un corte y un transitorio cada ~5,9 s en las
capturas decimadas.

Ahora la ISR de superMaquina, al recibir DONE con la captura incompleta, carga
el objetivo siguiente y pulsa START_NOW ahí mismo. Desde ST_DONE,
`superMaquina.v` pasa directo a ST_SAMPLING, bastante antes de la próxima
muestra (384 µs). El ADC no se detiene y no hay tráfico con el ESP. El camino
viejo del lazo principal (`capture_rearm_next_chunk`) queda como respaldo. El
watchdog de captura ya cubría la duración total.

Aplicado en los dos PSoC. `Serial_HAMMER` compila; el I2C principal no compila
hoy por su TopDesign.

## Datos medidos de las espigas (17-sep)

Captura por el maestro, nodo 2, SD habilitada; solo llegaron 689 muestras
(`probar_maestro.py`, COM6 se trabó después). Ruido de fondo σ≈64 cuentas. Hay
grupos de golpes separados por **150 muestras exactas** (152→302→…→602), el
tamaño de un sector de SD. La firma más común son tres golpes a +0, +22 y +41
muestras, de hasta ±180 cuentas, y se repite idéntica 450 muestras después.
Encaja con la hipótesis de la SD, pero la muestra es chica.

El código de SD es el mismo en las dos líneas: ambos PSoC habilitan la captura
a SD al arrancar si montan la tarjeta. Si la placa serial no mostraba las
espigas, la diferencia es de placa: tarjeta ausente, alimentación o layout, o
ganancia más baja.

## Marcas de escritura a SD y limpieza por software (17-sep)

Regla del usuario: nunca cortar el ADC ni perder muestras; antes sucias que
perdidas, pero con lo necesario para limpiarlas.

Recorrido de la marca:

1. **PSoC** (los dos): antes de cada `f_write` del drenaje, `sd_mark(SD_MARK_WRITE)`;
   antes de cada `f_sync`, `SD_MARK_SYNC`. La ISR (`sd_ring_store`) pega la
   marca pendiente a la PRÓXIMA muestra guardada: bits 21-22 del int24 en XOR
   contra el signo. El ADC es de 18 bits, así que ahí solo hay extensión de
   signo. `capture_reset_locked` limpia la marca pendiente.
2. **ESP** (I2C y serial): `decodeSdMarks()` sobre los lotes leídos de la SD
   (0xBF) restaura el valor exacto y pone `SampleBytes.flags`
   (`SAMPLE_FLAG_SD_WRITE` 0x01, `SAMPLE_FLAG_SD_SYNC` 0x02). Solo se aplica a
   lotes de SD, no a la rampa debug.
3. **Maestro**: por cada muestra con flags emite antes el paquete
   `[0x56][nodo][0xFB][flags][0][0]`. MATLAB y el scope de Python lo ignoran.
4. **Web**: `nd.sdMarks` guarda [índice, flags]; los snapshots y el ZIP lo
   exportan como `sd_marks`, con índices relativos a `raw_f32le.bin`.
5. **PC**: `src/interfaces/python/quitar_ruido_sd.py <carpeta_export>` escribe
   `raw_limpio_f32le.bin`. Pruebas: `test_quitar_ruido_sd.py`.

Validación de la ida y vuelta de bits: exacta para todo el rango de 18 bits y
las cuatro combinaciones de marca.

Validación con señal REAL de geófono (`src/interfaces/python/validar_ruido_sd_geofono.py`,
63 trazas de Canchiga con la firma medida inyectada; error en 10-50 Hz, % del rms
de la señal):

| caso | mediana | p90 |
|---|---|---|
| sucia | 0,69 | 1,21 |
| plantilla sacada de la misma captura | 1,64 | 3,03 (EMPEORA) |
| plantilla medida en banco (60 s) | 0,17 | 0,29 |

Conclusiones:
- En la banda útil, el ruido de la SD es chico frente a un golpe real (<1 %). La
  firma es sobre todo de alta frecuencia y queda por debajo del ruido de campo
  (~4 mV de banda ancha).
- La plantilla NO se puede estimar de la captura de campo: con ~58 escrituras la
  mediana arrastra ruido y la resta empeora. Por eso `quitar_ruido_sd.py` tiene
  dos pasos: `medir` una vez en banco (geófono quieto, SD activa, >=100
  escrituras) y `limpiar` con esa plantilla fija.
- Figura: `src/interfaces/python/datos/ruido_sd/validacion_geofono.png`.

Todo compila salvo el PSoC I2C principal (TopDesign). Compatibilidad: un ESP
nuevo con un PSoC viejo no ve marcas y no pasa nada. Un PSoC nuevo con un ESP
viejo deja una muestra cada 150 corrida en ±2^21 cuentas en las capturas a SD:
hay que grabarlos juntos.

## Ganancia de arranque (17-sep)

Síntoma: la placa I2C aparecía en PGA x1 / PGAout x1.

Causas:
1. El PSoC I2C arranca el hardware con el default del TopDesign (código 0), se
   lo informa al ESP en el saludo y recién después `control_init` aplica el
   perfil (x4/x24, `control_config.c`) sin avisar.
2. Los esclavos no mandan heartbeat: la web mostraba la ganancia guardada en el
   navegador, no la del PSoC.

Arreglo:
- ESP (los dos): `serviceBootGains()` fija la ganancia 4 s después de cada
  arranque del PSoC o del ESP y la confirma por el camino normal de config.
  I2C: `SLAVE_BOOT_PGA_CODE=2` (x4) y `SLAVE_BOOT_PGAOUT_CODE=5` (x24, solo si el
  PSoC es GEO). Serial: `SLAVE_BOOT_PGA_CODE=8` (x50).
- HELLO: dos bytes nuevos al final, `pga_code` y `pgaout_code`. El serial
  rellena `ctl_status=0xFF` para no correr los campos. El maestro los reenvía
  como sub 0x0A/0x0B y la web actualiza el panel y lo guardado, salvo que haya
  un cambio del usuario en vuelo. Web `field-study-23`.
- PSoC I2C: después de `control_init` manda los acks de 0xA6/0xA8 (sin compilar,
  por el TopDesign). PSoC serial: `g_pga_code = 8` antes de arrancar la cadena
  analógica (compila).

## Verificación en placa del arreglo de la SD (17-sep, noche)

Con todo grabado (PSoC, esclavo y maestro), captura de 60 lotes por el maestro
con la SD **habilitada**: 1800 muestras, sin espigas periódicas. Las pocas
muestras que superan el umbral están separadas 582, 241, 407, 133 y 106
muestras, o sea sin período; las de la mañana caían cada 150 exactas.

La captura entra en la RAM del PSoC (60 < 512 lotes), así que la SD no se toca
durante el muestreo. Esa es la regla nueva y quedó verificada de punta a punta.

Señal: media 53355 cuentas, sigma 294, pico a pico 2695, con PGA x4 y PGAout
x24 aplicados al arrancar.

## Decimación y el tope sin SD (2026-09-18)

Síntoma reportado: «con decimación 2 y 6 s termina usando la SD».

Cadena verificada de punta a punta:

- El PSoC decima con **media de bloque**, no descarte: `g_decim_acc += val;
  val = g_decim_acc / g_capture_decim_factor` (`main.c:1535-1539`). Guarda la
  muestra ya promediada, así que un lote son 30 muestras *decimadas*.
- El esclavo decide SD con `n_batches > PSOC_CAPTURE_MAX_BATCHES` (512) en
  `allocStore()`, y esos lotes son de muestras decimadas ⇒ **el tope en
  segundos escala solo con N**: 512·30/fs. Con N=1 son 5,90 s; con N=2, 11,80 s;
  con N=10, 59,07 s. No hubo que cambiar nada del esclavo.
- El PSoC informa la fs efectiva (`psoc_adc_effective_fs_hz()` = nativa/N) y
  manda FS_REPORT tras `PSOC_CMD_SET_DECIMATION`; el esclavo la refleja en
  `psoc.sampleRate()` y la publica en HELLO sub 0x05. Medido en placa:
  `decim 5 → 520 Hz`, `decim 2 → 1302 Hz`.

**La falla estaba entera en la web** y eran dos cosas:

1. Mientras el nodo retiene una captura sin descargar (`state=4 fill=n/n`) deja
   de emitir HELLO, así que la fs de la página se queda en la nativa. Con 2604 Hz,
   6 s ⇒ 521 lotes > 512 ⇒ SD. Con la fs correcta (1302 Hz) son 261 lotes ⇒ RAM.
   Arreglo: el ACK ok=1 de decimación aplica ya la fs efectiva
   (`applyGlobalFs(floor(2604/N))`) sin esperar al HELLO, que después la refina.
2. `applyGlobalFs()` no llamaba a `updateCapturePreview()`, así que al cambiar la
   fs el renglón de lotes seguía mostrando el cálculo anterior.

Además el preview ahora dice explícitamente `· sin SD (tope X,XX s)` o
`· USA SD (sin SD hasta X,XX s)`, para que el límite se vea sin tener que
deducirlo. Versión de caché de la web: `field-study-24`.

**Trampa de laboratorio:** abrir COM6 desde un script (`probar_maestro.py`,
`fs_tras_decim.py`) resetea el ESP maestro y le arranca la sesión a la página
abierta — START y los botones dejan de responder hasta recargar con F5. No
sondear el maestro por USB mientras alguien usa la web.

## Autoridad del maestro sobre la configuración (2026-09-18)

Pedido: «el maestro debe forzar su autoridad en la configuración de cada esclavo
si difiere; el maestro desempata».

La N es propiedad del ARREGLO, no de cada placa: una captura sincronizada exige
que todos compartan Fs. El maestro guarda la última N ordenada (`g_wantDecim`,
se fija cuando reenvía un `SUBCMD_DECIMATION` con `err == ESP_OK`) y en
`serviceSlaveAuthority()` compara contra la Fs que cada HELLO informa. Si
difiere, la vuelve a imponer: uno por vuelta de `loop()`, cada 5 s como mucho, y
**se rinde a los 5 intentos** — si un nodo no puede cumplir (otra frecuencia
nativa) insistir para siempre sería ruido de radio inútil; queda en el log
(`AUTHORITY_DECIM`). Solo actúa con el maestro en IDLE o ARMED.

Probado de punta a punta contra el nodo 2: se ordena decim 4 desde el maestro
(Fs 651), se desincroniza el nodo a mano por su consola USB (`decim 1`, sin pasar
por el maestro) y el maestro se la vuelve a imponer sola dentro de los 10 s.

## Cortes de cordura del volcado (2026-09-18)

Un esclavo que capturó 0 lotes dejaba al maestro pidiendo cada lote
`DUMP_MAX_RETRIES` (30) x `DUMP_BATCH_TIMEOUT_MS` (300) = 9 s: con 261 lotes
vacíos son **39 minutos** sin atender nada más, que desde afuera se ve como
«el maestro se colgó en DUMPING». Y la pausa por falta de cliente WS no tenía
final: una pestaña cerrada lo dejaba ahí para siempre.

Ahora `abortDump()` corta por dos lados: tres lotes seguidos sin respuesta
(`DUMP_MAX_SKIP_STREAK`) o 30 s sin cliente web (`DUMP_WS_WAIT_MAX_MS`).

**Trampa del banco:** el comando dirigido por USB son **6 bytes**
`[0xAB][0xBD][nodo][sub][param][xor]`; los 7 bytes son solo para el IDAC manual
(0xAA). `probar_maestro.directed7()` sirve para 0xAA y **no** para el resto: con
7 bytes el maestro descarta el comando en silencio y parece que el esclavo lo
ignoró. Me costó un diagnóstico falso.
