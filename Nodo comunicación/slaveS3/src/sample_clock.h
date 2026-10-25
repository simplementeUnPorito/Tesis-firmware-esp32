#pragma once

#include <Arduino.h>

namespace sample_clock {

void begin();
void service();
void armStart(int64_t localEspUs, uint32_t masterMacUs, bool masterValid);
void printStatus();
uint32_t nominalRateMilliHz();
bool enabled();

}  // namespace sample_clock
