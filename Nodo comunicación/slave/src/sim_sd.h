#pragma once

#if defined(SIM_SIN_HW) && SIM_SIN_HW

#include <FS.h>
#include <SPI.h>
#include <stdint.h>

/* SD simulada sobre la particion de datos "spiffs" de
 * particiones_ota_16mb.csv. Los archivos y sus bytes no cambian: solo cambia
 * el backend que implementa fs::FS. */
bool simSdBegin();
void simSdEnd();
fs::FS &simSdFs();
uint64_t simSdFreeBytes();

class SimSdClass {
public:
    bool begin(uint8_t cs, SPIClass &spi, uint32_t hz, const char *mountpoint,
               uint8_t maxFiles, bool formatIfEmpty);
    void end();
    File open(const char *path, const char *mode);
    uint64_t totalBytes();
    uint64_t usedBytes();
};

extern SimSdClass SimSD;

#endif
