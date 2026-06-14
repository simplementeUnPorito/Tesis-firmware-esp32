7# Handoff: Sincronización PSoC↔ESP — jitter de arranque y techo de frecuencia para MASW

Resumen de una sesión de revisión de código (sin cambios aplicados todavía) sobre
la cadena de disparo de muestreo `master ESP → esclavo ESP → PSoC`, pensando en
qué tan robusta es para análisis de ondas Rayleigh (MASW). Pensado para que un
agente fresco (o vos mañana) pueda retomar sin releer todo el hilo.

---

## 0. Contexto del proyecto (para quien no lo tenga)

**Objetivo:** evaluar robustez de la arquitectura de sincronización y determinar
la frecuencia máxima razonable para análisis de ondas Rayleigh.

**Hardware:**
- Geófono SM-24: fn=10Hz, spurious>240Hz, sensibilidad=85.8V/m/s, Rbobina≈375Ω
- PSoC 5LP: ADC Delta-Sigma 18 bits, fs=3000Hz (configurable 500-3000Hz)
- ESP32: ESP-NOW broadcast sin router, GPIO trigger hacia PSoC

**Cadena analógica (por nodo):**
- Geófono → CCLD propio → HPF: 47µF + 68k a Vref (fc≈0.05Hz)
- Nodo PGAin: 390Ω serie + Zeners antiparalelo + 1µF a Vss (LPF≈410Hz)
- PGA PSoC (0-24dB en pasos de 6dB) → Delta-Sigma 18bits
- Filtro anti-alias: Bessel 3er orden fc=300Hz (hardware analógico)
- Sin filtrado digital en PSoC — datos raw a PC para post-proceso en MATLAB

**Parámetros de señal:**
- Banda útil objetivo: 5-50Hz ondas Rayleigh (MASW)
- Geófono físicamente limitado a <240Hz (spurious)
- Bessel 300Hz >> banda útil → no limita señal de interés
- fs=3000Hz → Nyquist=1500Hz → anti-alias interno delta-sigma irrelevante para señal sísmica

**Protocolo de sincronización (descripción inicial del usuario, a contrastar con el código):**
1. Master pregunta uno a uno a esclavos → cada uno responde "listo" (unicast)
2. Master espera todos los ACK + 5ms fijos
3. Master broadcast START (FF:FF:FF:FF:FF:FF) → sin ACK, sin retransmisión
4. Esclavos reciben START → ISR GPIO → PSoC arranca DMA/ADC
- Esclavos completamente idle hasta recibir START
- Jitter ESP-NOW broadcast medido: <400µs (lab, ~7m)
- Literatura broadcast sin router: ~60-80µs entre receptores (TU Darmstadt 2025)

**Repos revisados:**
- PSoC: `src/psoc/AcondicionamientoAnalogico.cydsn/main.c`
- ESP: `src/esp/Nodo comunicación/{master,slave}/src/main.cpp`

---

## 1. Handshake de arranque: PRESTART → ARMED → SYNC_IN → SAMPLING

Pregunta original: *"¿el PSoC queda esperando a que le llegue la señal de su ESP
cuando se hace el pre-start?"*

Sí. Secuencia confirmada en código:

1. **Pre-start (`CMD_PRESTART` 0x61, ESP-NOW maestro→esclavo)** → el ESP esclavo
   entra a `enterHotWait()` (`slave/src/main.cpp:285-303`):
   - `psoc.setN(n_batches)` → manda `0xA3` por UART al PSoC
   - `psoc.preStart()` → manda `0xB1` por UART al PSoC (`slave/src/psoc_uart.cpp:182`)

2. **PSoC recibe `0xB1`** (`AcondicionamientoAnalogico.cydsn/main.c:477-478`) →
   `psoc_arm()`:
   ```c
   static void psoc_arm(void)
   {
       ADC_StopConvert();
       capture_reset_locked();
       g_state = PSOC_ARMED;   // <- queda armado, NO muestrea todavía
   }
   ```

3. **Disparo real**: NO llega por UART, sino por el pin **SYNC_IN** del PSoC
   (GPIO físico). `isr_SyncIn` (`main.c:280-303`):
   ```c
   CY_ISR(isr_SyncIn)
   {
       if (SYNC_IN_Read())
       {
           if (g_state == PSOC_ARMED)
               psoc_enter_sampling(g_debug_psoc);  // recién aquí -> PSOC_SAMPLING
       }
       ...
   }
   ```

4. Ese pin lo controla el ESP esclavo (`SYNC_TO_PSOC_PIN`, GPIO23 → PSoC `SYNC_IN`
   = P12[6]), puesto en HIGH en `onDataRecv` al recibir `CMD_START` por ESP-NOW
   (no hay cable hardware maestro→esclavo activo — ver sección 3).

**Conclusión:** tras el pre-start el PSoC está en `PSOC_ARMED`, sigue atendiendo
UART normalmente, pero el disparo de muestreo es por flanco GPIO `SYNC_IN`
(controlado por el ESP), no por otro mensaje UART.

---

## 2. ¿Convendría un loop de polling en vez de ISR para bajar varianza?

Pregunta: *"¿no sería mejor que el PSoC quedase en un loop esperando, para
disminuir varianza?"*

Conclusión de la discusión (sin cambios hechos):
- La latencia de `isr_SyncIn` en Cortex-M3 a 24MHz es de pocos ciclos
  (sub-microsegundo); solo se degrada si coincide con `isr_Timer` (10ms,
  handler corto) o una RX de UART → variación del orden de decenas de ciclos
  (nanosegundos).
- Un polling activo *podría* bajar eso a casi cero, pero obliga a deshabilitar
  UART/timer mientras se espera (pierde watchdog y "desarmar" por software) —
  complejidad para una ganancia probablemente insignificante.
- **Recomendación:** no vale la pena tocar esto a menos que se mida que el
  jitter del PSoC es el término dominante. El término dominante real está del
  lado ESP (ver sección 3-4).

---

## 3. Análisis de las 5 preguntas (revisión de código, sin medir hardware)

### P1 — ¿La ISR del PSoC hace algo más que habilitar DMA? ¿Es determinística?

**No hay DMA en este firmware.** El muestreo es 100% por interrupción por
muestra (`isr_DelSigReady`, `main.c:237-260`, lee `ADC_GetResult32()` muestra a
muestra). `isr_SyncIn` → `psoc_enter_sampling()` (`main.c:224-235`) hace:

```c
static void psoc_enter_sampling(uint8 debugMode)
{
    ADC_StopConvert();
    timer_stop_quiet();          // Timer_Stop + ReadStatusRegister + ClearPending
    saved = CyEnterCriticalSection();
    capture_reset_locked();      // resetea 7 variables volatile
    g_state = PSOC_SAMPLING;
    CyExitCriticalSection(saved);
    ADC_StartConvert();
}
```

- Determinística en el camino de código (sin ramas por datos).
- Pero `ADC_StopConvert()`+`ADC_StartConvert()` reinicia el pipeline del
  Delta-Sigma → retardo de grupo fijo (varios períodos del modulador) antes de
  la primera muestra válida. Igual en todos los nodos (no agrega jitter
  *entre* nodos), pero **nunca medido/calibrado** — desplaza el "t=0 real"
  respecto al flanco SYNC.
- Jitter residual: si `isr_Timer` (10ms) corre justo cuando llega el flanco,
  `isr_SyncIn` se demora el tiempo de ese handler (<1µs a 24MHz). Pequeño.

### P2 — ¿`onDataRecv` del ESP solo levanta GPIO o hace más cosas?

**No.** Es el hallazgo más importante. En `slave/src/main.cpp:453-489`,
handler de `CMD_START`:

```c
sendStartAck(1, startToken, nowUs);   // <-- esp_now_send() ANTES del GPIO
digitalWrite(SYNC_TO_PSOC_PIN, HIGH); // <-- esto dispara al PSoC
g_t_start_us    = msg->t_start_us;
...
g_state = SAMPLING;
```

- `onDataRecv` **no es una ISR de GPIO** — es el callback de recepción
  ESP-NOW, corre en la **tarea WiFi de FreeRTOS**, compitiendo con otras
  tareas (web server, MATLAB UART, `psoc.poll()`).
- Antes de tocar `SYNC_TO_PSOC_PIN` se llama `sendStartAck()` →
  `espnowSend()` → `esp_now_send()` (driver WiFi: locks, copia de buffer, cola
  TX) → **latencia variable y no acotada** insertada antes del flanco que ve
  el PSoC.
- El único camino "ISR GPIO" real (`onSyncEdge`, `IRAM_ATTR`,
  `slave/src/main.cpp:167-184`) **no está cableado** en el flujo actual:
  `SYNC_OUT_PIN` del maestro es solo marcador de osciloscopio
  (`master/README.md`, `platformio.ini`: *"el flanco del maestro (SYNC_OUT) no
  está cableado a los esclavos"*). El arranque real depende 100% del camino
  software ESP-NOW.

### P3 — ¿Hay latencia variable ESP32→PSoC que domine sobre el jitter ESP-NOW?

Muy probablemente sí. El jitter `<400µs` medido es de **llegada del paquete**
(nivel radio). El camino completo es:

```
radio RX → tarea WiFi despacha onDataRecv → checks de estado →
sendStartAck() [esp_now_send, variable] → digitalWrite(SYNC_TO_PSOC) →
isr_SyncIn (PSoC) → ADC_Stop/Start (pipeline delay fijo) → primera muestra
```

El tramo "tarea WiFi despacha → ACK por radio → GPIO" **no está medido** y,
al depender de la carga de cada ESP en ese instante, es potencialmente
**distinto por nodo** (a diferencia del jitter de broadcast, que es simétrico
entre receptores). Este es el componente que más probablemente domine la
varianza *inter-nodo*, justo la que arruina MASW (errores de fase entre
geófonos).

Dato útil: `MsgStartAck` ya devuelve `rx_us` (timestamp de `onDataRecv` al
entrar — `sync_protocol.h`, `slave/src/main.cpp:266-270`). Permite calibrar
**radio+despacho**, pero NO calibra `sendStartAck()`→`digitalWrite` ni el
pipeline del ADC del PSoC — los dos tramos de P1/P2.

### P4 — ¿Es robusta la arquitectura completa para MASW?

- **Diseño conceptual bueno**: PRESTART/HOT_WAIT saca todo el trabajo pesado
  (`setN`, arm del PSoC, alocación de buffers, query de listos) **fuera** del
  camino crítico de START — al llegar `CMD_START` cada esclavo solo necesita
  levantar un pin.
- **Punto débil concreto**: el orden `sendStartAck()` → `digitalWrite()` en
  `onDataRecv` (slave/src/main.cpp:481-482) mete una transmisión de radio en
  el camino crítico de sincronización sin necesidad. Es una corrección de
  implementación simple (invertir el orden).

### P5 — ¿Hasta qué frecuencia es razonable medir?

Con `fs=3000Hz` (período ≈ 333µs) y banda objetivo 5-50Hz:

- Error de fase a frecuencia *f* por desfase Δt entre nodos: `Δφ = 2π·f·Δt`.
- Para Δφ < 5° (≈0.087 rad): a f=50Hz → Δt < ~280µs; a f=100Hz → Δt < ~140µs.

Sin medir el tramo "ACK antes de GPIO" (P2/P3), no se puede afirmar que el
jitter inter-nodo esté por debajo de esos 140-280µs. Recomendación:

- Para **5-50Hz** (objetivo MASW): si tras el fix de P4 el jitter medido queda
  bajo ~200-300µs, el sistema es razonable.
- **No extender el análisis con confianza más allá de ~50-100Hz** sin: (a)
  reordenar ACK/GPIO, (b) medir jitter inter-nodo real en `SYNC_TO_PSOC_PIN`
  con osciloscopio (3 nodos simultáneos), y (c) caracterizar el retardo de
  pipeline del Delta-Sigma del PSoC.

---

## 4. Próximos pasos propuestos (no aplicados aún)

1. **Reordenar** en `slave/src/main.cpp` (`onDataRecv`, caso `CMD_START`, y el
   equivalente en `finishPrestartAction` / VER si aplica): hacer
   `digitalWrite(SYNC_TO_PSOC_PIN, HIGH)` **antes** de `sendStartAck(...)`.
2. **Instrumentar** un pin de debug que se togglee al entrar a `onDataRecv`
   (antes de cualquier otra cosa) para medir en el osciloscopio, en los 3
   nodos a la vez:
   - Δt entre "paquete CMD_START recibido" y "flanco en `SYNC_TO_PSOC_PIN`"
     (antes y después del fix del punto 1)
   - Δt inter-nodo del flanco final en `SYNC_TO_PSOC_PIN`
3. **Caracterizar** el retardo de pipeline del Delta-Sigma del PSoC (offset
   fijo entre `ADC_StartConvert()` y la primera `isr_DelSigReady`), para tener
   el presupuesto de tiempo completo.
4. Con los números reales, recalcular el techo de frecuencia (fórmula de P5)
   y decidir si hace falta compensación por software (usar `rx_us` +
   timestamps medidos para corregir `t_start_us` por nodo en post-proceso).
