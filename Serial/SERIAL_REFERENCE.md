# Referencia serial HAMMER

Esta carpeta es la rama de trabajo serial del esclavo ESP32, basada en la
copia recuperada de Git.

- Commit: `adf41af1e002821cd970bc6a2468cafe4dcd58a8`
- Fecha: 2026-07-21 00:17:24 -0300
- Pareja PSoC: `806a58094af489d6a936f1b19f17a5b8c124d204`
- Enlace ESP-PSoC: UART serial

## Cableado confirmado

- ESP GPIO16 (TX) -> PSoC P1[5] (`Rx`)
- ESP GPIO17 (RX) <- PSoC P1[2] (`Tx`)
- ESP GPIO23 (SYNC) -> PSoC P12[2] (`SYNC_IN`)

Los valores originales están en `platformio.ini`: `PSOC_UART_TX=16`,
`PSOC_UART_RX=17` y `SYNC_TO_PSOC_PIN=23`.

La lógica PSoC serial, VDAC y de calibración se conserva. Sobre esta base solo
se agregó el metadato aditivo `link_model=SERIAL` al HELLO para que el maestro
nuevo seleccione la interfaz correcta. La copia histórica exacta, sin esa
adaptación, queda en `../Serial_HAMMER`.

También se copió `src/scope_measurement.h` desde el mismo commit y se ajustó
su `#include`: originalmente vivía fuera de `slave`, por lo que el proyecto
recuperado no compilaba de manera independiente.
