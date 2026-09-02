# Firmware ESP32 de Tesis

Firmware PlatformIO del gateway maestro y de los nodos esclavos ESP32 del sistema MASW. Este repositorio conserva el historial que antes vivía en `src/esp`.

El subsistema principal está en [`Nodo comunicación/`](./Nodo%20comunicación/README.md):

- `master/`: gateway USB/Wi-Fi/ESP-NOW e interfaz web embebida.
- `slave/`: enlace ESP-NOW con el maestro; UART ESP→PSoC, I2C PSoC→ESP y
  sincronismo dedicado con el PSoC.
- scripts de prueba y simulación del protocolo.

La placa completa se valida con el entorno `slaveTest`; su protocolo,
comandos, pinout y evidencia están documentados en
`src/firmware/psoc/AUTOTEST_NODO_ESCLAVO.md` dentro del superproyecto (repositorio
`Tesis-firmware-psoc`). El README propio del esclavo contiene el flujo corto de
compilación y uso.

## Clonado y compilación

```powershell
git clone https://github.com/simplementeUnPorito/Tesis-firmware-esp32.git
cd "Tesis-firmware-esp32/Nodo comunicación/master"
pio run -e esp32dev
```

Para los esclavos, ejecutar PlatformIO desde `Nodo comunicación/slave`. Los controladores CP210x son software externo y ya no se guardan en Git; deben instalarse desde Silicon Labs cuando Windows no reconozca el puerto serie.

En el superproyecto `Tesis` este repositorio se monta en
`src/firmware/esp32`.
