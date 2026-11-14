/**
 * @file power_diag.h
 * @brief Deep-sleep diagnostics: battery drain and clock drift per sleep cycle
 *
 * Each deep sleep records a snapshot in RTC memory; the next boot turns it into a
 * record (sleep length by the ESP32 RC clock and by the PCF8563, battery before and
 * after) kept in NVS namespace "pwrdiag". The PCF8563 is written from SNTP and used
 * to restore the system clock after a wake, since the RC clock drifts a few percent.
 */

#ifndef POWER_DIAG_H
#define POWER_DIAG_H

#include <cstdint>
#include <string>

namespace ink {
namespace PowerDiag {

/** @brief Call once early in startup, after the board is constructed */
void OnBoot();
/**
 * @brief Call right before esp_deep_sleep_start()
 * @param planned_wake_s wall-clock seconds until the intended wake; 0 if no timer
 * @param programmed_s seconds actually programmed into the ESP32 RC timer
 */
void BeforeDeepSleep(uint32_t planned_wake_s, uint32_t programmed_s = 0);
/** @brief Call from the SNTP sync callback; work runs on a background task */
void OnSntpSynced();
/** @brief History, cumulative estimate and PCF8563 drift as JSON for /power_log */
std::string HistoryJson();

/** @brief RC/PCF sleep-duration scale, typically ~1.01 when the RC clock runs fast */
float RcSleepScale();
/** @brief Convert a wall-clock sleep into RC-timer seconds (at least 1 when wall_s > 0) */
uint32_t CompensateSleepSec(uint32_t wall_s);

}  // namespace PowerDiag
}  // namespace ink

#endif  // POWER_DIAG_H
