#include "sd_storage.h"
#include "psoc_uart.h"   /* SPI_BATCH_SAMPLES */

#ifdef SD_SPI_CS_PIN

#ifndef SD_SPI_SCK_PIN
  #error "SD_SPI_CS_PIN definido sin SD_SPI_SCK_PIN/MISO/MOSI en platformio.ini"
#endif

#include <SPI.h>
#include <SD.h>

/* Un solo archivo de sesión: el store-and-forward de este firmware nunca
 * tiene dos capturas "vivas" a la vez (store-then-dump), así que no hace
 * falta nombrar archivos por captura — se recrea en cada allocStore(). */
static const char SD_CAPTURE_FILENAME[] = "/capture.bin";

/* Registro por lote: 30 muestras (10 bytes c/u) + timestamp relativo (4 bytes). */
static const uint32_t SD_RECORD_BYTES =
    (uint32_t)SPI_BATCH_SAMPLES * sizeof(SampleBytes) + sizeof(uint32_t);

/* Tope defensivo: acotar sdStorageMaxBatches() a algo que siga entrando en un
 * uint16_t de n_batches con margen (65535), aunque la tarjeta sea enorme. */
static const uint32_t SD_MAX_BATCHES_HARD_CAP = 60000u;

static SPIClass g_sdSpi(VSPI);
static bool     g_sdPresent = false;
static bool     g_sdEnabled = false;
static File     g_sdFile;

void sdStorageBegin()
{
    g_sdSpi.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    g_sdPresent = SD.begin(SD_SPI_CS_PIN, g_sdSpi);
    g_sdEnabled = false;   /* reposo seguro: nunca arranca grabando a SD sin que lo pidan */
}

bool sdStoragePresent() { return g_sdPresent; }
bool sdStorageEnabled() { return g_sdEnabled; }

bool sdStorageSetEnabled(bool enable)
{
    if (enable && !g_sdPresent) return false;
    g_sdEnabled = enable;
    return true;
}

uint32_t sdStorageMaxBatches()
{
    if (!g_sdPresent || !g_sdEnabled) return 0;
    uint64_t cardBytes = (uint64_t)SD.cardSize();
    if (cardBytes == 0) return 0;
    uint64_t maxBatches = cardBytes / (uint64_t)SD_RECORD_BYTES;
    if (maxBatches > SD_MAX_BATCHES_HARD_CAP) maxBatches = SD_MAX_BATCHES_HARD_CAP;
    return (uint32_t)maxBatches;
}

bool sdStorageBeginSession(uint16_t total_batches)
{
    if (!g_sdPresent || !g_sdEnabled) return false;
    if (g_sdFile) g_sdFile.close();
    SD.remove(SD_CAPTURE_FILENAME);
    g_sdFile = SD.open(SD_CAPTURE_FILENAME, FILE_WRITE);
    if (!g_sdFile) return false;
    (void)total_batches;   /* no se preasigna tamaño: SD.h/FAT no lo requiere */
    return true;
}

bool sdStorageWriteBatch(uint16_t seq, const SampleBytes *samples, uint32_t ts_us)
{
    if (!g_sdFile) return false;
    uint32_t offset = (uint32_t)seq * SD_RECORD_BYTES;
    if (!g_sdFile.seek(offset)) return false;
    size_t wrote = g_sdFile.write((const uint8_t *)samples,
                                   (size_t)SPI_BATCH_SAMPLES * sizeof(SampleBytes));
    wrote += g_sdFile.write((const uint8_t *)&ts_us, sizeof(ts_us));
    return wrote == SD_RECORD_BYTES;
}

bool sdStorageReadBatch(uint16_t seq, SampleBytes *outSamples, uint32_t *outTsUs)
{
    if (!g_sdFile) return false;
    uint32_t offset = (uint32_t)seq * SD_RECORD_BYTES;
    if (!g_sdFile.seek(offset)) return false;
    size_t got = g_sdFile.read((uint8_t *)outSamples,
                                (size_t)SPI_BATCH_SAMPLES * sizeof(SampleBytes));
    got += g_sdFile.read((uint8_t *)outTsUs, sizeof(*outTsUs));
    return got == SD_RECORD_BYTES;
}

void sdStorageEndSession()
{
    if (g_sdFile) g_sdFile.close();
}

#else /* !SD_SPI_CS_PIN: firmware sin módulo SD cableado (HAMMER hoy) — stubs sin efecto */

void     sdStorageBegin() {}
bool     sdStoragePresent() { return false; }
bool     sdStorageEnabled() { return false; }
bool     sdStorageSetEnabled(bool /*enable*/) { return false; }
uint32_t sdStorageMaxBatches() { return 0; }
bool     sdStorageBeginSession(uint16_t /*total_batches*/) { return false; }
bool     sdStorageWriteBatch(uint16_t /*seq*/, const SampleBytes * /*samples*/, uint32_t /*ts_us*/) { return false; }
bool     sdStorageReadBatch(uint16_t /*seq*/, SampleBytes * /*outSamples*/, uint32_t * /*outTsUs*/) { return false; }
void     sdStorageEndSession() {}

#endif
