#include "sample_clock.h"
#include "ftm_slave.h"
#include "remote_log.h"
#include <math.h>

#ifndef SOC_OUT_PIN
#define SOC_OUT_PIN -1
#endif
#ifndef SOC_PERIOD_NS
#define SOC_PERIOD_NS 500000ULL
#endif
#ifndef PSOC_CLK_OUT_ENABLE
#define PSOC_CLK_OUT_ENABLE 0
#endif
#ifndef PSOC_CLK_OUT_PIN
#define PSOC_CLK_OUT_PIN -1
#endif

#if defined(ARDUINO_ESP32S3_DEV) && SOC_OUT_PIN >= 0
#include <driver/mcpwm_cmpr.h>
#include <driver/mcpwm_gen.h>
#include <driver/mcpwm_oper.h>
#include <driver/mcpwm_timer.h>
#include <esp_timer.h>

namespace sample_clock {
namespace {

constexpr uint32_t TIMER_HZ = 160000000u;
constexpr uint32_t PULSE_NS = 3000u;
constexpr uint32_t SERVICE_DIV = 32u;
constexpr int64_t FP_SCALE = 65536LL;
constexpr int32_t PHASE_CORR_LIMIT_NS = 500;
constexpr int32_t ARM_CORR_LIMIT_NS = 10000;

mcpwm_timer_handle_t s_timer = nullptr;
mcpwm_oper_handle_t s_oper = nullptr;
mcpwm_cmpr_handle_t s_compare = nullptr;
mcpwm_gen_handle_t s_generator = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint64_t s_cycles = 0;
volatile int64_t s_lastEdgeEspUs = 0;
volatile bool s_serviceDue = false;
int64_t s_peakFp = 0;
int64_t s_fracAcc = 0;
double s_integralNs = 0.0;
int32_t s_phaseErrorNs = 0;
int32_t s_phaseCorrectionNs = 0;
int32_t s_driftPpb = 0;
uint32_t s_modelUncertNs = 0;
bool s_modelGap = true;
volatile int64_t s_armLocalUs = 0;
uint32_t s_armMasterUs = 0;
bool s_armMasterValid = false;
volatile bool s_armPending = false;
volatile bool s_restoreAfterArm = false;

int32_t signedPhaseNs(int64_t timeNs)
{
    const int64_t period = (int64_t)SOC_PERIOD_NS;
    int64_t phase = timeNs % period;
    if (phase < 0) phase += period;
    if (phase >= period / 2) phase -= period;
    return (int32_t)phase;
}

bool IRAM_ATTR onEmpty(mcpwm_timer_handle_t,
                       const mcpwm_timer_event_data_t *, void *)
{
    portENTER_CRITICAL_ISR(&s_mux);
    s_cycles++;
    s_lastEdgeEspUs = esp_timer_get_time();
    int64_t peak = s_peakFp / FP_SCALE;
    s_fracAcc += s_peakFp % FP_SCALE;
    if (s_fracAcc >= FP_SCALE) {
        peak++;
        s_fracAcc -= FP_SCALE;
    }
    if (peak < 2) peak = 2;
    if (peak > 65535) peak = 65535;
    if (s_restoreAfterArm) {
        s_restoreAfterArm = false;
    } else if (s_armPending) {
        const int64_t deltaUs = s_armLocalUs - s_lastEdgeEspUs;
        const int64_t alignPeak = deltaUs * (TIMER_HZ / 1000000u) / 2;
        /* Igual que el DTR de referencia: se modifica un único período en el
         * borde anterior y después se vuelve al período disciplinado. */
        if (alignPeak > (int64_t)(PULSE_NS * TIMER_HZ / 1000000000ULL + 2u) &&
            alignPeak <= 65535) {
            peak = alignPeak;
            s_armPending = false;
            s_restoreAfterArm = true;
        } else if (deltaUs <= 0) {
            s_armPending = false;
        }
    }
    if ((s_cycles % SERVICE_DIV) == 0u) s_serviceDue = true;
    portEXIT_CRITICAL_ISR(&s_mux);
    mcpwm_timer_set_period(s_timer, (uint32_t)peak);
    return false;
}

void setPeriodModel(int32_t driftPpb, int32_t phaseCorrectionNs)
{
    const double fullPeriodNs = (double)SOC_PERIOD_NS * (1.0 + (double)driftPpb * 1e-9)
                              + (double)phaseCorrectionNs;
    const double peakTicks = fullPeriodNs * ((double)TIMER_HZ / 1e9) / 2.0;
    int64_t fp = (int64_t)llround(peakTicks * (double)FP_SCALE);
    const int64_t lo = 2LL * FP_SCALE;
    const int64_t hi = 65535LL * FP_SCALE;
    if (fp < lo) fp = lo;
    if (fp > hi) fp = hi;
    portENTER_CRITICAL(&s_mux);
    s_peakFp = fp;
    portEXIT_CRITICAL(&s_mux);
}

}  // namespace

void begin()
{
#if PSOC_CLK_OUT_ENABLE && PSOC_CLK_OUT_PIN >= 0
    /* Arduino 3 usa la API LEDC por pin. Un bit da exactamente 50 % a 8 MHz. */
    if (ledcAttach(PSOC_CLK_OUT_PIN, 8000000u, 1u)) ledcWrite(PSOC_CLK_OUT_PIN, 1u);
#endif
    const uint32_t peakTicks = (uint32_t)(((uint64_t)SOC_PERIOD_NS * TIMER_HZ) / 2000000000ULL);
    mcpwm_timer_config_t timerCfg = {};
    timerCfg.group_id = 0;
    timerCfg.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    timerCfg.resolution_hz = TIMER_HZ;
    timerCfg.count_mode = MCPWM_TIMER_COUNT_MODE_UP_DOWN;
    timerCfg.period_ticks = peakTicks;
    timerCfg.intr_priority = 3;
    timerCfg.flags.update_period_on_empty = true;
    if (mcpwm_new_timer(&timerCfg, &s_timer) != ESP_OK) return;
    mcpwm_timer_event_callbacks_t callbacks = {};
    callbacks.on_empty = onEmpty;
    if (mcpwm_timer_register_event_callbacks(s_timer, &callbacks, nullptr) != ESP_OK) return;

    mcpwm_operator_config_t operCfg = {};
    operCfg.group_id = 0;
    if (mcpwm_new_operator(&operCfg, &s_oper) != ESP_OK) return;
    if (mcpwm_operator_connect_timer(s_oper, s_timer) != ESP_OK) return;
    mcpwm_comparator_config_t compareCfg = {};
    compareCfg.flags.update_cmp_on_tez = true;
    if (mcpwm_new_comparator(s_oper, &compareCfg, &s_compare) != ESP_OK) return;
    const uint32_t pulseTicks = (uint32_t)(((uint64_t)PULSE_NS * TIMER_HZ) / 1000000000ULL);
    mcpwm_comparator_set_compare_value(s_compare, pulseTicks);
    mcpwm_generator_config_t generatorCfg = {};
    generatorCfg.gen_gpio_num = SOC_OUT_PIN;
    if (mcpwm_new_generator(s_oper, &generatorCfg, &s_generator) != ESP_OK) return;
    mcpwm_generator_set_action_on_timer_event(
        s_generator, MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                   MCPWM_TIMER_EVENT_EMPTY,
                                                   MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(
        s_generator, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                     s_compare,
                                                     MCPWM_GEN_ACTION_LOW));
    s_peakFp = (int64_t)peakTicks * FP_SCALE;
    mcpwm_timer_enable(s_timer);
    mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP);
    Serial.printf("[SOC] MCPWM GPIO%d periodo=%llu ns pulso=%u ns resol=6.25 ns\n",
                  SOC_OUT_PIN, (unsigned long long)SOC_PERIOD_NS, PULSE_NS);
}

void armStart(int64_t localEspUs, uint32_t masterMacUs, bool masterValid)
{
    portENTER_CRITICAL(&s_mux);
    s_armLocalUs = localEspUs;
    s_armMasterUs = masterMacUs;
    s_armMasterValid = masterValid;
    s_armPending = s_timer != nullptr && localEspUs > esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
}

void service()
{
    if (!s_timer || !s_serviceDue) return;
    int64_t edgeEsp;
    portENTER_CRITICAL(&s_mux);
    s_serviceDue = false;
    edgeEsp = s_lastEdgeEspUs;
    portEXIT_CRITICAL(&s_mux);

    uint32_t masterUs = 0;
    const bool fresh = ftm::modelOk();
    const bool converted = ftm::localEspToMasterMac(edgeEsp, masterUs);
    int32_t errorNs = s_phaseErrorNs;
    if (converted) {
        /* La fase pertenece a esta captura: T0 + k*periodo. En un START sin
         * modelo se usa primero el ancla local y, cuando aparece un modelo
         * fresco, se convierte ese mismo T0 una sola vez. La resta uint32
         * conserva el tiempo transcurrido al cruzar el wrap MAC (capturas <
         * 71,6 min). */
        if (!s_armMasterValid && s_armLocalUs != 0 && fresh) {
            uint32_t armMasterUs = 0;
            if (ftm::localEspToMasterMac(s_armLocalUs, armMasterUs)) {
                s_armMasterUs = armMasterUs;
                s_armMasterValid = true;
            }
        }
        if (s_armMasterValid) {
            const uint32_t elapsedMasterUs = masterUs - s_armMasterUs;
            errorNs = signedPhaseNs((int64_t)elapsedMasterUs * 1000LL);
        } else if (s_armLocalUs != 0) {
            errorNs = signedPhaseNs((edgeEsp - s_armLocalUs) * 1000LL);
        } else {
            errorNs = signedPhaseNs((int64_t)masterUs * 1000LL);
        }
        s_driftPpb = ftm::modelSkewPpb();
        s_modelUncertNs = ftm::modelResidualNs();
    } else if (s_armLocalUs != 0) {
        errorNs = signedPhaseNs((edgeEsp - s_armLocalUs) * 1000LL);
    }
    s_modelGap = !fresh;
    s_phaseErrorNs = errorNs;

    const bool armedSoon = s_armLocalUs != 0 && edgeEsp < s_armLocalUs + 1000000LL;
    const int32_t limit = armedSoon ? ARM_CORR_LIMIT_NS : PHASE_CORR_LIMIT_NS;
    s_integralNs += (double)errorNs * 0.0005;
    if (s_integralNs > 200.0) s_integralNs = 200.0;
    if (s_integralNs < -200.0) s_integralNs = -200.0;
    int32_t correction = (int32_t)llround(-(double)errorNs * 0.02 - s_integralNs);
    if (correction > limit) correction = limit;
    if (correction < -limit) correction = -limit;
    s_phaseCorrectionNs = correction;
    setPeriodModel(s_driftPpb, correction);
}

void printStatus()
{
    double peak;
    portENTER_CRITICAL(&s_mux);
    peak = (double)s_peakFp / FP_SCALE;
    const uint64_t cycles = s_cycles;
    portEXIT_CRITICAL(&s_mux);
    const double periodNs = peak * 2.0 * 1e9 / TIMER_HZ;
    Serial.printf("#SOCCLK on=1 pin=%d periodo_ns=%.3f fase_ns=%ld corr_ns=%ld deriva_ppb=%ld resid_ns=%lu modelo=%u hueco=%u ciclos=%llu\n",
                  SOC_OUT_PIN, periodNs, (long)s_phaseErrorNs,
                  (long)s_phaseCorrectionNs, (long)s_driftPpb,
                  (unsigned long)s_modelUncertNs, (unsigned)ftm::modelOk(),
                  (unsigned)s_modelGap, (unsigned long long)cycles);
}

uint32_t nominalRateMilliHz()
{
    return (uint32_t)(1000000000000ULL / SOC_PERIOD_NS);
}

bool enabled() { return s_timer != nullptr; }

}  // namespace sample_clock

#else

namespace sample_clock {
void begin() {}
void service() {}
void armStart(int64_t, uint32_t, bool) {}
void printStatus() { Serial.print("#SOCCLK on=0\n"); }
uint32_t nominalRateMilliHz() { return 0u; }
bool enabled() { return false; }
}  // namespace sample_clock

#endif
