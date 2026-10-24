#pragma once

#include <Arduino.h>

#if SLAVE_SELFTEST

enum StVerdict : uint8_t { ST_V_PASS = 0, ST_V_FAIL, ST_V_WARN, ST_V_SKIP, ST_V_INFO };

void stReportBegin(const char *espBuild, const char *psocBuild,
                   const char *mac, uint8_t nodeId);
void stReportItem(const char *code, const char *name, StVerdict v, const char *fmt, ...);
void stReportSummary();
void stReportReset();
uint16_t stReportCount(StVerdict v);
bool     stReportAnyFail();
const char *stReportFirstFail();

/* Devuelve true si algun item FAIL o WARN tiene un codigo que empieza con
 * `prefix`. Lo usa el bloque de diagnostico para agrupar D6.0..D6.3 bajo D6. */
bool stReportHasIssueWithPrefix(const char *prefix);

#else

enum StVerdict : uint8_t { ST_V_PASS = 0, ST_V_FAIL, ST_V_WARN, ST_V_SKIP, ST_V_INFO };

// Los stubs inline permiten que el compilador elimine tambien las llamadas del
// firmware de campo, sin sumar simbolos ni arrastrar codigo de presentacion.
inline void stReportBegin(const char *, const char *, const char *, uint8_t) {}
inline void stReportItem(const char *, const char *, StVerdict, const char *, ...) {}
inline void stReportSummary() {}
inline void stReportReset() {}
inline uint16_t stReportCount(StVerdict) { return 0; }
inline bool stReportAnyFail() { return false; }
inline const char *stReportFirstFail() { return nullptr; }
inline bool stReportHasIssueWithPrefix(const char *) { return false; }

#endif
