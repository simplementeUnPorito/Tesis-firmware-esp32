#if SLAVE_SELFTEST

#include "selftest_report.h"

#include <stdarg.h>
#include <stdio.h>

namespace {

constexpr uint8_t kVerdictCount = 5;
constexpr uint8_t kMaxListedCodes = 16;
constexpr size_t kStoredCodeSize = 8;
constexpr size_t kDetailSize = 97;
constexpr size_t kItemLabelWidth = 44;

uint16_t s_counts[kVerdictCount] = {};
char s_failCodes[kMaxListedCodes][kStoredCodeSize] = {};
char s_warnCodes[kMaxListedCodes][kStoredCodeSize] = {};
uint8_t s_failCodeCount = 0;
uint8_t s_warnCodeCount = 0;

const char *safeText(const char *text) {
  return text != nullptr ? text : "";
}

size_t boundedLength(const char *text, size_t maximum) {
  size_t length = 0;
  text = safeText(text);
  while (length < maximum && text[length] != '\0') {
    ++length;
  }
  return length;
}

StVerdict normalizedVerdict(StVerdict verdict) {
  const uint8_t value = static_cast<uint8_t>(verdict);
  return value < kVerdictCount ? verdict : ST_V_INFO;
}

const char *verdictText(StVerdict verdict) {
  static const char *const labels[kVerdictCount] = {
      "PASS", "FAIL", "WARN", "SKIP", "INFO"};
  return labels[static_cast<uint8_t>(normalizedVerdict(verdict))];
}

void rememberCode(char codes[][kStoredCodeSize], uint8_t &count,
                  const char *code) {
  if (count >= kMaxListedCodes) {
    return;
  }

  // El limite fijo evita que un codigo inesperado agrande el estado retenido.
  snprintf(codes[count], kStoredCodeSize, "%s", safeText(code));
  ++count;
}

void formatItemLabel(char (&label)[kItemLabelWidth + 1], const char *code,
                     const char *name) {
  const char *safeCode = safeText(code);
  const size_t codeLength = boundedLength(safeCode, kStoredCodeSize - 1);
  const size_t prefixLength = codeLength + 3;
  const size_t nameCapacity = kItemLabelWidth - prefixLength - 2;

  int written = snprintf(label, sizeof(label), "[%.*s] %.*s",
                         static_cast<int>(codeLength), safeCode,
                         static_cast<int>(nameCapacity), safeText(name));
  size_t position = written > 0 ? static_cast<size_t>(written) : 0;
  if (position > kItemLabelWidth - 2) {
    position = kItemLabelWidth - 2;
  }

  // Se reserva al menos un punto: aun con nombres largos queda visible que la
  // columna de veredictos esta alineada y no es parte del nombre truncado.
  label[position++] = ' ';
  while (position < kItemLabelWidth - 1) {
    label[position++] = '.';
  }
  label[kItemLabelWidth - 1] = ' ';
  label[kItemLabelWidth] = '\0';
}

void printCodeList(const char *heading,
                   const char codes[][kStoredCodeSize], uint8_t count) {
  char line[160];
  size_t used = static_cast<size_t>(
      snprintf(line, sizeof(line), "%s:", safeText(heading)));

  for (uint8_t i = 0; i < count && used < sizeof(line); ++i) {
    const int appended = snprintf(line + used, sizeof(line) - used, "%s%s",
                                  i == 0 ? " " : ", ", codes[i]);
    if (appended < 0) {
      break;
    }
    used += static_cast<size_t>(appended);
  }

  Serial.println(line);
}

void formatJsonString(const char *input, char (&output)[45]) {
  size_t used = 0;
  output[used++] = '"';

  input = safeText(input);
  for (size_t i = 0; input[i] != '\0' && used + 7 < sizeof(output); ++i) {
    const uint8_t value = static_cast<uint8_t>(input[i]);
    if (value == '"' || value == '\\') {
      output[used++] = '\\';
      output[used++] = static_cast<char>(value);
    } else if (value < 0x20) {
      const int written = snprintf(output + used, sizeof(output) - used,
                                   "\\u%04x", static_cast<unsigned>(value));
      if (written != 6) {
        break;
      }
      used += 6;
    } else {
      output[used++] = static_cast<char>(value);
    }
  }

  output[used++] = '"';
  output[used] = '\0';
}

void printJsonCodes(const char codes[][kStoredCodeSize], uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    char escaped[45];
    char piece[48];
    formatJsonString(codes[i], escaped);
    snprintf(piece, sizeof(piece), "%s%s", i == 0 ? "" : ",", escaped);
    Serial.print(piece);
  }
}

}

void stReportBegin(const char *espBuild, const char *psocBuild,
                   const char *mac, uint8_t nodeId) {
  char line[128];

  // Serial ya lo abrio setup(): reabrirlo aca tira los bytes en vuelo.
  stReportReset();

  snprintf(line, sizeof(line), "=========== AUTOTEST NODO ESCLAVO ===========");
  Serial.println(line);
  snprintf(line, sizeof(line), "ESP  %s   MAC %s   NODE_ID=%u", safeText(espBuild),
           safeText(mac), static_cast<unsigned>(nodeId));
  Serial.println(line);
  snprintf(line, sizeof(line), "PSoC %s", safeText(psocBuild));
  Serial.println(line);
}

void stReportItem(const char *code, const char *name, StVerdict verdict,
                  const char *fmt, ...) {
  char detail[kDetailSize];
  char label[kItemLabelWidth + 1];
  char line[kItemLabelWidth + 1 + 4 + 2 + kDetailSize];

  detail[0] = '\0';
  if (fmt != nullptr) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);
  }

  verdict = normalizedVerdict(verdict);
  ++s_counts[static_cast<uint8_t>(verdict)];
  if (verdict == ST_V_FAIL) {
    rememberCode(s_failCodes, s_failCodeCount, code);
  } else if (verdict == ST_V_WARN) {
    rememberCode(s_warnCodes, s_warnCodeCount, code);
  }

  formatItemLabel(label, code, name);
  if (detail[0] != '\0') {
    snprintf(line, sizeof(line), "%s%s  %s", label, verdictText(verdict), detail);
  } else {
    snprintf(line, sizeof(line), "%s%s", label, verdictText(verdict));
  }
  Serial.println(line);
}

void stReportSummary() {
  char line[192];
  const bool failed = stReportAnyFail();

  snprintf(line, sizeof(line), "%s", "");
  Serial.println(line);
  snprintf(line, sizeof(line),
           "RESUMEN  %u PASS - %u FAIL - %u WARN - %u SKIP - %u INFO"
           "     VEREDICTO: %s",
           static_cast<unsigned>(s_counts[ST_V_PASS]),
           static_cast<unsigned>(s_counts[ST_V_FAIL]),
           static_cast<unsigned>(s_counts[ST_V_WARN]),
           static_cast<unsigned>(s_counts[ST_V_SKIP]),
           static_cast<unsigned>(s_counts[ST_V_INFO]),
           failed ? "NO APTO" : "APTO");
  Serial.println(line);

  if (s_failCodeCount > 0) {
    printCodeList("FAIL", s_failCodes, s_failCodeCount);
  }
  if (s_warnCodeCount > 0) {
    printCodeList("WARN", s_warnCodes, s_warnCodeCount);
  }

  // El JSON se emite por partes pero con un unico salto al final. Asi sigue
  // siendo una sola linea sin reservar un buffer grande en el stack.
  snprintf(line, sizeof(line),
           "#JSON {\"verdict\":\"%s\",\"pass\":%u,\"fail\":[",
           failed ? "FAIL" : "PASS",
           static_cast<unsigned>(s_counts[ST_V_PASS]));
  Serial.print(line);
  printJsonCodes(s_failCodes, s_failCodeCount);
  snprintf(line, sizeof(line), "],\"warn\":[");
  Serial.print(line);
  printJsonCodes(s_warnCodes, s_warnCodeCount);
  snprintf(line, sizeof(line), "],\"skip\":%u,\"info\":%u}",
           static_cast<unsigned>(s_counts[ST_V_SKIP]),
           static_cast<unsigned>(s_counts[ST_V_INFO]));
  Serial.println(line);

  snprintf(line, sizeof(line), "=============================================");
  Serial.println(line);
}

void stReportReset() {
  for (uint8_t i = 0; i < kVerdictCount; ++i) {
    s_counts[i] = 0;
  }
  s_failCodeCount = 0;
  s_warnCodeCount = 0;
}

uint16_t stReportCount(StVerdict verdict) {
  const uint8_t value = static_cast<uint8_t>(verdict);
  return value < kVerdictCount ? s_counts[value] : 0;
}

bool stReportAnyFail() {
  return s_counts[ST_V_FAIL] != 0;
}

const char *stReportFirstFail() {
  return s_failCodeCount > 0 ? s_failCodes[0] : nullptr;
}

#else

#include "selftest_report.h"

// Las implementaciones vacias viven inline en el header para no dejar codigo
// enlazable en el firmware de campo.

#endif
