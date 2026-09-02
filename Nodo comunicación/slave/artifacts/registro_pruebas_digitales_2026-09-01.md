# Registro de puesta en marcha digital — placa nodo esclavo

Fecha local: 2026-09-01 22:55 (-03:00)  
Rama: `cambios-hardware`  
Repositorio: `10f7d37d` · ESP32: `128d778` · PSoC: `b122d64`

## Alcance

Se verificó únicamente la parte digital de la placa. La cadena analógica, su
calibración y cualquier conclusión basada en amplitud o ruido quedan fuera de
esta sesión.

## Hardware y firmware usados

- ESP32 por COM8, entorno PlatformIO `slaveTest`.
- PSoC 5LP `CY8C5888LTI-LP097` mediante KitProg
  `KitProg/1D1F17F002152400`.
- Proyecto PSoC correcto:
  `AcondicionamientoAnalogicoTest/AcondicionamientoAnalogico.cydsn/AcondicionamientoAnalogico.cywrk`.
- HEX PSoC:
  `AcondicionamientoAnalogicoTest/AcondicionamientoAnalogico.cydsn/CortexM3/ARM_GCC_541/Debug/AcondicionamientoAnalogico.hex`.
- El usuario programó ese proyecto desde PSoC Creator. Creator informó
  programación exitosa a las 22:44:33.

## Diagnóstico y correcciones realizadas

1. La reparación GPIO26 del ESP32 → P15[0] del PSoC quedó confirmada por B2:
   respuesta STATUS de ida por UART y vuelta por I2C en 54 ms.
2. Había continuidad entre GPIO27 y P0[4], pero B3 daba cero flancos porque el
   contador pertenecía a la ISR `isr_SyncIn` antigua. El diseño actual entrega
   SYNC a `superMaquina`; esa ISR ya no es la dueña de la señal.
3. Se confirmó que P0[4] está correctamente configurado como entrada
   `High impedance digital`. B3 se cambió para que el PSoC lea directamente el
   nivel físico con GPIO27 en LOW y luego en HIGH.
4. B0b daba un FAIL histórico si el PSoC había sido programado después del
   arranque del ESP. Ahora la prueba usa el tráfico I2C actual de B1 como fuente
   de verdad.
5. C4 habilitaba la rampa antes de fijar N y provocaba una captura espuria de
   128 lotes. Eso dejaba ocupado al PSoC y generaba falsos fallos posteriores en
   C5, C6 y C7. Se creó una secuencia de captura de debug que primero fija N=8,
   captura exactamente 8 lotes y después deshabilita el modo debug.
6. Se agregó diagnóstico SD desde el PSoC: etapa de inicialización, último R1,
   niveles observados en los cuatro pads SPI y flags de error.
7. El firmware ESP de autotest ya no ejecuta automáticamente la corrida
   completa al arrancar. Los grupos digitales se lanzan con `b` y `c`; `run`
   conserva la corrida completa para cuando se pruebe lo analógico.
8. Se corrigió `program_psoc.ps1`: admite `-SelfTest` y programa las cuatro
   matrices de 256 filas. En PSoC 5LP el ruteo/configuración digital vive en el
   espacio ECC, por lo que no alcanza con programar solo las filas ocupadas por
   código.

## Resultado digital final

| Prueba | Resultado | Evidencia |
|---|---:|---|
| B0, reposo I2C | PASS | SDA y SCL en alto; pull-ups externos presentes |
| B1, PSoC → ESP por I2C | PASS | 8 B/1,5 s; 2 pings; 0 overruns |
| B2a, TX del ESP | PASS | 131 flancos en GPIO26; repetición: 129 |
| B2, ESP → PSoC → ESP | PASS | STATUS respondido en 54 ms |
| B3, GPIO27 → P0[4] | PASS | P0[4] leyó LOW y HIGH correctamente |
| B4, integridad de trama | PASS | `bBad=0`, `badLen=0` |
| C1, identidad PSoC | PASS | GEO, 2604 Hz, 4 etapas, 5 canales AMux |
| C2, IRQ/HardFault | PASS | 0 IRQ inesperadas, 0 HardFaults |
| C4, captura digital E2E | PASS | 8/8 lotes, 240 muestras, 100 % crecientes |
| C6, SD/FatFs | PASS | FAT montado; escritura y lectura correctas |
| C7, pulsador PSoC en reposo | PASS | nivel 1 estable en 5/5 lecturas |
| E1.0, botón ESP UP | PASS | pulsación y liberación detectadas en GPIO34 |
| E1.1, botón ESP DOWN | PASS | pulsación y liberación detectadas en GPIO35 |
| E1.2, botón ESP OK | PASS | pulsación y liberación detectadas en GPIO36 |
| E1.3, botón ESP BACK | PASS | pulsación y liberación detectadas en GPIO39 |
| E2, botón del PSoC | PASS | cambio de nivel detectado en P2[2] |

Diagnóstico SD final:

- Estado `0x3F`, tipo 3.
- Inicialización completa (`lista`).
- Respuesta R1 `0x00`.
- CS, SCK, MOSI y MISO observaron ambos niveles (`0/1`).
- Flags de error `0x00`.

## Corridas finales

### Corrida limpia después de reset del PSoC

- Grupo B: **8 PASS, 0 FAIL, 0 WARN, 0 SKIP, 1 INFO — APTO**.
- Grupo C: **5 PASS, 0 FAIL, 1 WARN, 1 SKIP, 1 INFO — APTO**.

### Prueba adicional de repetibilidad, sin reiniciar

Se repitieron B y C completos, incluido un segundo ciclo de escritura/lectura
SD. Los resultados fueron idénticos:

- Grupo B: **8 PASS, 0 FAIL — APTO**.
- Grupo C: **5 PASS, 0 FAIL — APTO**.
- SD volvió a responder con R1 `0x00`, pads `0/1`, error `0x00` y lectura/
  escritura correctas.

### Pruebas interactivas de botones

- Botón del PSoC P2[2]: **1 PASS, 0 FAIL — APTO**.
- Botones ESP UP/DOWN/OK/BACK: **4 PASS, 0 FAIL — APTO**.
- En los cuatro botones del ESP se verificaron tanto la pulsación como la
  liberación; ninguno quedó retenido en LOW.

## Advertencias que no indican falla digital de la placa

- C3: `0/9` slots EEPROM calibrados. Es esperable antes de calibrar la cadena
  analógica.
- C5/DFB: `SKIP` porque el RMS crudo fue 1,6 counts y luego 1,3 counts, demasiado
  bajo para comparar atenuación. El DFB es interno al PSoC y no es un componente
  soldado de la placa; no se tomó este SKIP como defecto de hardware.

## Compilación y pruebas de software

- PSoC Creator, firmware normal: **Build Succeeded**, 60536 bytes de flash y
  51928 bytes de SRAM.
- PSoC Creator, firmware autotest: **Build Succeeded**, 67240 bytes de flash y
  17384 bytes de SRAM.
- PlatformIO `slave2`: **SUCCESS**, 799201 bytes de flash y 49160 bytes de RAM.
- PlatformIO `slaveTest`: **SUCCESS**, 823409 bytes de flash y 49564 bytes de RAM.
- ESP32 programado correctamente por COM8.
- `autotest_runner.py --self-test`: **28/28 PASS**.
- `test_autotest_format.py`: **17/17 PASS**.
- `git diff --check`: sin errores; solo advertencias de conversión LF/CRLF.

## Conclusión

La parte digital soldada de la placa queda **APTA**. No se requiere medición con
tester ni osciloscopio para I2C, UART, SYNC o SD. La parte analógica y la
calibración se dejan explícitamente para otra sesión.
