#pragma once

/* Ancho del pulso unico de START para osciloscopio, en microsegundos.
 * Aplica a:
 *   - master GPIO15
 *   - slave1 GPIO32
 *   - slave2 GPIO22
 */
#ifndef SCOPE_START_PULSE_US
  #define SCOPE_START_PULSE_US 1000
#endif
