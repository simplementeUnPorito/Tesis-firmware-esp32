#include "sim_sd.h"

#if defined(SIM_SIN_HW) && SIM_SIN_HW

#include <LittleFS.h>

namespace {
bool g_mounted = false;
}

SimSdClass SimSD;

bool simSdBegin()
{
    if (!g_mounted) {
        g_mounted = LittleFS.begin(true, "/sd", 10, "spiffs");
    }
    return g_mounted;
}

void simSdEnd()
{
    /* LittleFS es compartido por toda la aplicacion. Mantenerlo montado evita
     * invalidar handles ajenos; para live_capture equivale a SD.end(). */
}

fs::FS &simSdFs() { return LittleFS; }

uint64_t simSdFreeBytes()
{
    if (!g_mounted) return 0u;
    const uint64_t total = LittleFS.totalBytes();
    const uint64_t used = LittleFS.usedBytes();
    return (used < total) ? (total - used) : 0u;
}

bool SimSdClass::begin(uint8_t, SPIClass &, uint32_t, const char *, uint8_t, bool)
{
    return simSdBegin();
}

void SimSdClass::end() { simSdEnd(); }

File SimSdClass::open(const char *path, const char *mode)
{
    return simSdFs().open(path, mode);
}

uint64_t SimSdClass::totalBytes()
{
    return g_mounted ? LittleFS.totalBytes() : 0u;
}

uint64_t SimSdClass::usedBytes()
{
    return g_mounted ? LittleFS.usedBytes() : 0u;
}

#endif
