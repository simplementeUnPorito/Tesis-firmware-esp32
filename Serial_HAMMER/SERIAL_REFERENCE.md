# Referencia serial HAMMER

Esta carpeta es una copia de trabajo del esclavo ESP32 recuperada de Git.

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

Esta copia se conserva separada del firmware actual para acondicionarla luego
al protocolo común sin alterar el snapshot histórico ni el trabajo en curso.
