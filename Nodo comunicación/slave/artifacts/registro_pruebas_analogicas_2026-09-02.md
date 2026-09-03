# Registro de verificación de la cadena analógica — placa nodo esclavo

Fecha local: 2026-09-02 21:40–22:05 (-03:00)
Rama: `main` · ESP32: `071682b` · PSoC: `f130fd0`

## Alcance

Cadena analógica completa por autotest (grupos D1–D8) más los grupos A/B/C que
la sostienen, **incluido D7 con golpe real** y la comparación de D6b con y sin
geófono. Los pulsadores (E1, E2) no se repitieron: quedaron validados el
2026-09-01.

## Hardware y firmware

- ESP32 por COM8, entorno PlatformIO `slaveTest`, recompilado a HEAD:
  823449 B de flash / 49564 B de RAM, `Hash of data verified` en la grabación.
- PSoC 5LP `CY8C5888LTI-LP097` por KitProg `KitProg/1D1F17F002152400`,
  proyecto `AcondicionamientoAnalogicoTest`, 67240 B de flash / 17384 B de SRAM.
- Perfil de hardware: `oled=0 btn=2 geo=1 sd=1 psoc=2`.

## Evidencia

- `artifacts/autotest_analogico_2026-09-02.json` — corrida completa 1.
- `artifacts/autotest_analogico_2026-09-02_r2.json` — corrida completa 2.
- Corrida intermedia de los grupos `c` y `d` sueltos, para repetibilidad.

## Regresión de herramienta: `program_psoc.ps1` deja el PSoC sin arrancar

`program_psoc.ps1 -SelfTest` compiló y grabó sin un solo error: las 4×256 filas
con `PSoC3_VerifyRowFromHex` en `0 OK`, más `ProtectAll`, `VerifyProtect`,
`DAP_ReleaseChip` y `ClosePort` en `0 OK`. Aun así el PSoC **no volvió a
arrancar**: cero flancos en SCL (`[B0b] FAIL`), silencio total en el I2C de
subida, con los pull-ups y el reposo del bus correctos. Dos `ToggleReset` por
KitProg no lo recuperaron.

Regrabando el mismo proyecto desde PSoC Creator el enlace volvió de inmediato
(`probe=1`, pings y diagnósticos entrando). El código es el mismo: el build del
script y el de Creator dan el mismo tamaño de flash y de SRAM. Lo que no queda
aplicado es la configuración del *fabric*, no el código.

Sospecha, sin confirmar: al script le falta escribir las NV latches. El HEX las
trae (`user 00 00 40 05`, `WO bc 90 ac af`, `nvlUserSize=4`, `nvlWoSize=4`) y el
script nunca las escribe. No se pudieron leer las del chip para comparar:
`PSoC3_ReadNvlArray` devuelve `0x80004005`.

**Hasta que se resuelva, grabar el PSoC desde PSoC Creator.**

## El firmware modela la portadora JitX, no la placa construida

`psoc_hw.h` deriva toda la relación código IDAC → tensión de:

    R de conversión = 30 kΩ,  Vref = 2.0622 V (AMS1117-ADJ con R22=1k, R23=620)
    LSB = 125 nA × 30 kΩ = 3.75 mV

Esos son los valores de la **portadora JitX, que todavía no se fabricó**. En
`red_analogica.py` son exactamente lo que aplica `aplicar_portadora()`, no los
valores por defecto. La placa construida, según el TopDesign y confirmado por
Elías el 2026-09-02:

- R11–R14 = 15 kΩ (R11 medida en 14,76 kΩ sobre seis lecturas, ver
  `debug_analogico/HALLAZGOS_ANALOGICO.md`), no 30 kΩ;
- las cuatro ramas vuelven a `Vref`, no a un riel `VREF_2V048`;
- `Vref` = **2,5 V**, y no lo genera un AMS1117 con divisor: es `Vdda/2`
  bufferado por `OPAref` (`Ref2V5`) en el propio TopDesign;
- los IDAC8 están en el rango 0–31,875 µA, 1/8 µA por bit;
- cada referencia vale **`Vref ± R·Idac`**, con el signo dado por
  `polarity_reg`: aproximadamente 2,0 a 3,0 V.

Consecuencias:

1. El LSB real de esta placa es **1,875 mV**, la mitad del que asume el firmware.
2. `psoc_idac_code_to_uv()` está mal en los dos términos. La pendiente usa
   30 kΩ en vez de 15 kΩ. El offset usa 2,0622 V calculados con tres constantes
   `PSOC_AMS1117_*` que describen un regulador que en esta placa no existe, en
   vez de los 2,5 V de `Vdda/2`. Y la función sólo suma: no sabe expresar el
   signo, así que no puede representar la mitad inferior del rango.
   La "tensión nominal" que devuelve el comando `0xA2` no es la de esta placa.
3. El umbral `TH_D2_MIN_SLOPE_UV_PER_CODE = 200` de `main_selftest.cpp` se
   justifica en el comentario con esos 3,75 mV/LSB. Para esta placa el umbral
   equivalente es **100 µV/código**.

Nota sobre las medidas de D1: los ~1010 mV que informa son **referidos a
`Vref`**, no tensiones absolutas. La cadena entra al ADC por un amplificador
cuya referencia es `Vdda/2`, así que todo lo que reporta el autotest en µV es
una desviación respecto de los 2,5 V.

## `polarity_reg` no se escribe nunca

El componente existe en `Generated_Source/PSoC5/polarity_reg.*` y sus cuatro
bits van a las entradas `ipolarity` de los cuatro IDAC8. **Ningún archivo del
firmware lo escribe**, ni el de campo ni el de autotest: cero referencias fuera
de lo generado. Los cuatro `ipolarity` quedan en el valor de reset del Control
Reg, de modo que las referencias sólo pueden alejarse de `Vref` en un sentido y
se pierde la mitad del rango.

Esto afecta a D2, que sólo puede barrer hacia un lado, y bloquea el port de la
calibración (D8) a IDAC8: para anular un offset hay que poder cruzar `Vref`.

## Resultado

### Digital

| Prueba | Resultado |
|---|---|
| A1, A3, A4 | PASS |
| B0, B0b, B0d, B1, B2, B2a, B3, B4 | PASS |
| C1, C2, C6, C7 | PASS |
| C3 | WARN — 0/9 slots de EEPROM: la placa sigue sin calibrar |
| C4, C5 | **intermitentes**, ver abajo |

`C6.1` volvió a dar el diagnóstico SD limpio: `init=lista R1=0x00`, los cuatro
pads con ambos niveles, `err=0x00`.

**C4/C5 no son estables.** En tres corridas con el mismo firmware:

| Corrida | C4 | C5 |
|---|---|---|
| 1 (completa) | PASS 8/8 lotes, 100 % crecientes | FAIL, no pudo capturar por ninguno de los dos caminos |
| 2 (grupo `c` suelto) | SKIP, sin SYNC armado por correr `c` sin `b` | SKIP, ídem |
| 3 (completa) | FAIL, la captura inmediata de rampa no entregó lotes | FAIL, `RMS FIR/crudo = 15.69` (670,6 vs 42,7) |

Que la misma prueba dé PASS y FAIL alternadamente apunta al armado de la
captura, no a una soldadura. El `15.69` de C5 es lo contrario de atenuar y
merece mirarse aparte; el DFB es interno al PSoC.

### Analógico

D1, D3 y los cuatro D6 pasan. D8 y D5 dan SKIP por diseño. Fallan D2 y D4.

Matriz de transferencia DC, en µV por código de IDAC, filas = etapa barrida,
columnas = tap medido (ch0 `PGAo`, ch1 `BPo`, ch2 `SUMo`, ch3 `LPo`):

| Etapa | ch0 | ch1 | ch2 | ch3 |
|---|---:|---:|---:|---:|
| 0 `Vref_PGA` | **61,0 / 61,5 / 61,0** | −67 / −61 / −67 | ~0 | disperso |
| 1 `Vref_BP` | −0,9 | **190,2 / 191,7 / 190,7** | −685,7 | +6414…6547 |
| 2 `Vref_ADDER` | ~2 | −6 | **767,2 / 768,7 / 770,1** | −6588…−6846 |
| 3 `Vref_LP` | ~0 | ~0 | ~0 | **1221 / 1270 / 1296** |

Las tres corridas coinciden dentro del 1 %: **esto no es ruido de medición.**

Tres cosas que dice la matriz:

1. **Ninguna etapa está muerta.** Cada una mueve su propio tap y todos los de
   aguas abajo, con el signo correcto y ganancia creciente. Una etapa muerta se
   ve distinto: en la corrida del 2026-09-01 la fila 3 era `−0,5 / 0 / 0 / −0,5`,
   todo cero. Hoy da 1270. Esa etapa se recuperó.
2. **El término cruzado confirma el modelo de la placa.** Barriendo `Vref_BP`,
   la relación entre ch2 y ch1 es −685,7/190,7 = −3,596. El circuito predice
   −R8/(R7+RV1) = −27k/7,48k = **−3,610**, usando los 680 Ω fijos en lugar del
   trimmer. Es una confirmación independiente del hallazgo 3 de
   `HALLAZGOS_ANALOGICO.md`: **RV1 no existe, hay un fijo de 680 Ω.**
3. **Con el umbral corregido a 100 µV/código sólo queda una etapa por debajo.**
   La etapa 1 (190 µV/código) pasa; la etapa 0, `Vref_PGA` → `PGAo`, con 61
   µV/código, no.

### D7 y D6b — el geófono

D7 se corrió con el geófono conectado, doce intentos seguidos, golpeando el
suelo al lado del sensor. **Tres capturas limpias, las tres PASS:**

| Intento | Pico | Fondo pp | Primera excursión |
|---:|---:|---:|---|
| 2 | 4246 counts | 295 | POSITIVA |
| 4 | 2584 counts | 235 | POSITIVA |
| 5 | 1173 counts | 436 | POSITIVA |

Los intentos fallidos no son fallas del sensor: siete dan pico 68–93 counts
contra un fondo de ~260, o sea capturas donde no hubo golpe dentro de la ventana
de 1,47 s, y uno (el 3) tiene fondo pp 6243 porque un golpe cayó dentro de la
ventana de fondo. **La polaridad salió POSITIVA en las tres capturas válidas.**

Ojo con el orden: `tap` suelto falla siempre con `no se pudo capturar el fondo`,
porque `stCapture()` devuelve false de entrada si `g_syncOk` es falso. Hay que
correr `b` antes en la misma sesión, sin reiniciar el ESP.

D6b quedó cerrado con el par de corridas:

| | Entrada abierta | Con geófono |
|---|---:|---:|
| `ch0` RMS | 19 µV | **38 µV** |
| `ch3` RMS | 801 µV | 419 µV |

El geófono **sube** el ruido de `ch0`, al revés de lo que anticipa el texto de
D6b. Ese texto razona sólo el ruido térmico: la bobina de 375 Ω en paralelo con
los ~102 kΩ de R2+R3 derrumba la impedancia del nodo y con ella el ruido de
Johnson. Pero el geófono no es un resistor: conectado transduce la vibración
ambiente del edificio, y eso pesa más que lo que ahorra en térmico. El
indicador sigue sirviendo —la diferencia es clara y repetible— pero **el signo
esperado que está escrito en `AUTOTEST_NODO_ESCLAVO.md` §6, D6b, está al revés**
y conviene corregirlo: un resistor de 375 Ω haría bajar el ruido; una bobina que
transduce, no.

### Barrido de ganancias de los dos PGA

Experimento posterior, con los comandos de laboratorio nuevos: fijar cada
ganancia y medir la **pendiente** de un barrido chico de IDAC (±8 códigos
alrededor de 128). El cociente de pendientes cancela el offset y la
incertidumbre de todos los resistores, así que mide la ganancia y nada más.

**PGA de entrada: anda bien.** Barriendo `Vref_PGA` y midiendo `ch0`:

| Código | Ganancia | Pendiente | Cociente medido | Esperado | Error |
|---:|---:|---:|---:|---:|---:|
| 0 | 1× | 57,7 µV/cód | — | — | — |
| 1 | 2× | 108,7 | 1,883 | 2 | 5,8 % |
| 2 | 4× | 220,2 | 3,816 | 4 | 4,6 % |
| 3 | 8× | 448,7 | 7,773 | 8 | 2,8 % |

**Esto reinterpreta el único FAIL analógico que quedaba en pie.** La etapa 0 da
61 µV/código en D2 porque el PGA está en ganancia 1×, que es su valor de
arranque (`PGAout_DEFAULT_GAIN = 0`). No es un resistor abierto ni una soldadura
fría: es el punto de trabajo. A 8× la misma etapa da 448,7 µV/código, muy por
encima de los 100 del umbral corregido y también de los 200 del firmware.

**PGAout: no se puede medir arriba de 1× en una placa sin calibrar.** Barriendo
`Vref_ADDER` y midiendo `ch3`:

| Código | Ganancia | Pendiente |
|---:|---:|---:|
| 0 | 1× | −3855,7 µV/cód |
| 1 | 2× | −0,9 |
| 2 | 4× | +0,5 |
| 3 | 8× | −0,5 |

La pendiente no baja: **se va a cero de golpe**. El mecanismo se ve en el reposo
DC: `ch2 SUMo` está a 0,75 V de `Vref`. PGAout amplifica esa diferencia, así que
a 2× su salida se va 1,5 V de `Vref` y a 4× unos 3 V, o sea contra el riel de
5 V. Con `PGAo` clavado contra el riel, `LPo = 6·Vref_LP − 5·PGAo` deja de
depender de la etapa que se está barriendo, y la pendiente medida es cero.

No es que PGAout esté roto: es que **sin calibrar, cualquier ganancia mayor que
1 lo satura**. Y calibrar es exactamente lo que haría D8, que está desactivado
porque su máquina no está portada a IDAC8 — y que además necesita
`polarity_reg`, que no se escribe nunca.

### D4 — el `cociente 0.00` no es una ganancia mal medida

D4 barre la etapa 2 y mide `g_slope[2][3]`, o sea `LPo`, con `PGAout` en 1× y en
4×. El informe dice `cociente 0.00`. Ese cero no sale de una ganancia chica:
sale de que a 4× el barrido **no devuelve medición** y `s4` queda en su valor
inicial, 0.

Que a 4× el tap se vaya contra el riel es lo esperado en una placa sin calibrar:
`LPo` ya está en 907 mV cuando los otros tres taps están en 1010–1030 mV, la
etapa de pasabajos tiene ganancia 1 + R10/R15 = 6 desde su referencia, y D5 da
SKIP diciendo justamente que el tap quedó fuera de ±0,45 V.

El firmware contempla el caso y quiere darle SKIP —el comentario lo dice—, pero
el guardia sólo cubre la saturación detectada por valor:

```c
for (uint8_t ch = 0; ch < nTaps; ch++) {
    if (!okLo[ch] || !okHi[ch]) { continue; }      /* medicion fallida: no marca sat */
    g_slopeSat[stage][ch] = isSaturated(vlo[ch]) || isSaturated(vhi[ch]);
}
```

Si la medición directamente falla, `g_slopeSat` queda en false, `sat` no se
activa, y D4 informa `cociente 0.00` como FAIL en vez del SKIP que pretendía.
Además `s4 = fabsf(g_slope[2][3])` se lee sin consultar `g_slopeOk[2][3]`.

**D4 no prueba hoy que la ganancia de `PGAout` esté mal.** Para que su veredicto
valga hace falta calibrar primero, que es lo que D8 haría si estuviera portado a
IDAC8.

## Conclusión sobre la cadena analógica

**No apareció ninguna falla física en el frente analógico.** Los tres FAIL
analógicos quedan explicados sin invocar un componente roto:

| FAIL | Qué era en realidad |
|---|---|
| D2, etapa 0 | El PGA de entrada está en 1×, su ganancia de arranque. A 8× esa misma etapa da 448,7 µV/código y pasa cualquiera de los dos umbrales. |
| D2, etapa 1 | 190 µV/código pasa con el umbral corregido a esta placa (100), no con el del firmware (200), que está calculado con la R de la portadora JitX. |
| D4 | PGAout satura arriba de 1× porque `SUMo` está a 0,75 V de `Vref` en una placa sin calibrar. El firmware quiere darle SKIP a ese caso pero su guardia no cubre la medición que no vuelve. |

Lo que la cadena sí necesita es **calibración**, y eso está bloqueado por dos
cosas concretas del firmware, no por la placa: la máquina de calibración sigue
siendo la de los VDAC8 y no está portada a IDAC8, y `polarity_reg` no se escribe
nunca, así que las referencias no pueden cruzar `Vref`.

## Qué queda abierto

1. **Portar la calibración a IDAC8 y escribir `polarity_reg`.** Es el
   desbloqueo: sin eso no hay D8, y sin D8 no se puede validar D4 ni el rango
   de ganancias útil.
2. **Corregir el modelo IDAC→tensión** a los valores de esta placa (R = 15 kΩ,
   `Vref` = 2,5 V, signo por `polarity_reg`), y con él el umbral de D2.
3. **El guardia de D4**: una medición que no vuelve tiene que dar SKIP, no un
   `cociente 0.00` que se lee como FAIL. Y `s4` se toma de `g_slope[2][3]` sin
   consultar `g_slopeOk[2][3]`.
4. **La escala absoluta no cierra con el modelo.** Con el LSB de 1,875 mV, la
   etapa 1 debería dar ganancia DC 1 desde su referencia hasta `BPo`, o sea
   ≈1875 µV/código; da 191. Un factor 10 sin explicar, y las cuatro etapas están
   bajas en la misma proporción. La sospecha es que `Vref` no es un nodo rígido
   y se mueve con la corriente inyectada; hace falta el esquemático completo
   para cerrarlo.
5. **Las etapas no son lineales en todo el rango.** El barrido completo de
   `Vref_LP` (0 a 240, paso 40, midiendo `ch3`) da zona muerta entre 0 y 40,
   tramo útil de ~1375 µV/código entre 80 y 160, y compresión arriba de 200
   (680 µV/código de 200 a 240). D2 mide con dos puntos separados 40 códigos, así
   que promedia zona muerta y compresión juntas. La calibración tiene que
   trabajar dentro del tramo útil.
6. **C4/C5 intermitentes**, ver arriba. Es lo único digital sin cerrar.
7. **`program_psoc.ps1` no debe usarse** hasta resolver lo de las NV latches.
