#include "power_diag.h"

#include "board.h"

#include <cJSON.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sys/time.h>

#include <atomic>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace ink {
namespace PowerDiag {

namespace {

constexpr char kTag[] = "PowerDiag";
constexpr char kNamespace[] = "pwrdiag";
constexpr char kRingKey[] = "ring";
constexpr char kSummaryKey[] = "sum";
constexpr char kPcfKey[] = "pcf";
constexpr char kRcScaleKey[] = "rc_scale";

constexpr uint32_t kSnapshotMagic = 0x50444732;  // "PDG2"
constexpr float kDefaultRcScale = 1.012f;
constexpr float kMinRcScale = 0.97f;
constexpr float kMaxRcScale = 1.06f;
constexpr int kRingSize = 24;
constexpr int kBatteryCapacityMah = 2000;
constexpr double kMinEstimateHours = 12.0;
constexpr int64_t kMinValidEpoch = 1735689600;  // 2025-01-01 UTC
constexpr uint32_t kMinPpmSpanSec = 3600;
constexpr int kPcfResyncThresholdMs = 2000;
constexpr int kEdgeWaitMs = 1100;

enum WakeCause : uint8_t { kWakeOther = 0, kWakeTimer = 1, kWakeButton = 2 };
enum RecordFlags : uint8_t { kFlagUsbPower = 1 << 0, kFlagPcfUsed = 1 << 1 };

struct SleepSnapshot {
    uint32_t magic;
    int64_t rc_epoch_ms;
    int64_t pcf_epoch;  // -1 when the PCF8563 was not usable
    uint32_t planned_s;      // wall-clock target
    uint32_t programmed_s;   // seconds written to the ESP32 RC timer
    uint32_t awake_s;
    uint16_t mv;
    uint8_t pct;
    uint8_t usb;
};

struct RcScaleState {
    float scale;
};
RTC_DATA_ATTR SleepSnapshot s_snapshot;

struct Record {
    uint32_t sleep_start;  // UTC epoch
    uint32_t rc_slept_s;
    int32_t pcf_slept_s;   // -1 when the PCF8563 was not usable at both ends
    uint32_t planned_s;
    uint32_t awake_s;      // awake time that preceded this sleep
    uint16_t mv_before;
    uint16_t mv_after;
    uint8_t pct_before;
    uint8_t pct_after;
    uint8_t wake_cause;
    uint8_t flags;
};

struct Ring {
    uint16_t head;  // next slot to write
    uint16_t count;
    Record records[kRingSize];
};

// Battery trend across consecutive deep-sleep cycles on battery power.
struct Summary {
    uint32_t start_epoch;
    uint32_t sleep_s;
    uint32_t awake_s;
    uint32_t cycles;
    uint16_t start_mv;
    uint16_t last_mv;
    uint8_t start_pct;
    uint8_t last_pct;
    uint8_t valid;
    uint8_t reserved;
};

struct PcfState {
    int64_t set_epoch;         // when SNTP last wrote the chip; 0 = never
    int64_t last_check_epoch;  // 0 = never compared against NTP
    int32_t last_offset_ms;    // PCF8563 minus NTP at the last check
    uint32_t last_span_s;      // seconds between the last write and that check
};

enum class PcfStatus { kOk, kReadFailed, kLostTime, kNeverSynced };

std::mutex s_mutex;
std::atomic<bool> s_sntp_task_running{false};

template <typename T>
bool LoadBlob(const char* key, T& out) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(T);
    const esp_err_t err = nvs_get_blob(handle, key, &out, &size);
    nvs_close(handle);
    return err == ESP_OK && size == sizeof(T);
}

template <typename T>
void SaveBlob(const char* key, const T& value) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, key, &value, sizeof(T)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

Ring LoadRing() {
    Ring ring = {};
    if (!LoadBlob(kRingKey, ring) || ring.head >= kRingSize || ring.count > kRingSize) {
        ring = {};
    }
    return ring;
}

Summary LoadSummary() {
    Summary summary = {};
    if (!LoadBlob(kSummaryKey, summary)) summary = {};
    return summary;
}

PcfState LoadPcf() {
    PcfState pcf = {};
    if (!LoadBlob(kPcfKey, pcf)) pcf = {};
    return pcf;
}

float ClampRcScale(float scale) {
    if (scale < kMinRcScale) return kMinRcScale;
    if (scale > kMaxRcScale) return kMaxRcScale;
    return scale;
}

float LoadRcScaleLocked() {
    RcScaleState state = {};
    if (!LoadBlob(kRcScaleKey, state) || state.scale < kMinRcScale || state.scale > kMaxRcScale) {
        return kDefaultRcScale;
    }
    return state.scale;
}

void SaveRcScaleLocked(float scale) {
    RcScaleState state = {};
    state.scale = ClampRcScale(scale);
    SaveBlob(kRcScaleKey, state);
}

void UpdateRcScaleLocked(float sample) {
    const float old_scale = LoadRcScaleLocked();
    const float next = ClampRcScale(0.3f * ClampRcScale(sample) + 0.7f * old_scale);
    SaveRcScaleLocked(next);
    ESP_LOGI(kTag, "RC scale sample %.4f, EWMA %.4f -> %.4f", sample, old_scale, next);
}

int64_t NowMs() {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

std::string FormatLocal(int64_t epoch) {
    if (epoch < kMinValidEpoch) return "unset";
    const time_t t = static_cast<time_t>(epoch);
    struct tm local_tm = {};
    localtime_r(&t, &local_tm);
    char buf[24];
    strftime(buf, sizeof(buf), "%m-%d %H:%M:%S", &local_tm);
    return buf;
}

const char* PcfStatusText(PcfStatus status) {
    switch (status) {
        case PcfStatus::kOk: return "ok";
        case PcfStatus::kReadFailed: return "I2C read failed";
        case PcfStatus::kLostTime: return "VL flag set, oscillator stopped";
        case PcfStatus::kNeverSynced: return "not yet written from SNTP";
    }
    return "?";
}

PcfStatus ReadPcfEpoch(const PcfState& pcf, int64_t& epoch) {
    time_t value = 0;
    bool valid = false;
    if (!Board::GetInstance().GetRtcTime(value, valid)) return PcfStatus::kReadFailed;
    if (!valid || value < kMinValidEpoch) return PcfStatus::kLostTime;
    if (pcf.set_epoch <= 0) return PcfStatus::kNeverSynced;
    epoch = value;
    return PcfStatus::kOk;
}

const char* WakeText(uint8_t cause) {
    switch (cause) {
        case kWakeTimer: return "timer";
        case kWakeButton: return "button";
        default: return "other";
    }
}

uint32_t SleptSeconds(const Record& r) {
    return r.pcf_slept_s >= 0 ? static_cast<uint32_t>(r.pcf_slept_s) : r.rc_slept_s;
}

bool RcDriftPercent(const Record& r, double& percent) {
    if (r.pcf_slept_s <= 0) return false;
    percent = (static_cast<double>(r.rc_slept_s) - r.pcf_slept_s) * 100.0 / r.pcf_slept_s;
    return true;
}

void UpdateSummary(Summary& sum, const Record& r) {
    if (r.flags & kFlagUsbPower) {
        sum = {};
        return;
    }
    if (!sum.valid) {
        // The awake time before the first sleep happened before start_mv was taken.
        sum = {};
        sum.valid = 1;
        sum.start_epoch = r.sleep_start;
        sum.start_mv = r.mv_before;
        sum.start_pct = r.pct_before;
    } else {
        sum.awake_s += r.awake_s;
    }
    sum.sleep_s += SleptSeconds(r);
    sum.cycles += 1;
    sum.last_mv = r.mv_after;
    sum.last_pct = r.pct_after;
}

struct Estimate {
    double hours = 0;
    double sleep_ratio = 0;
    double mv_per_hour = 0;
    double avg_ma = 0;
    bool reliable = false;
};

Estimate ComputeEstimate(const Summary& sum) {
    Estimate est;
    const uint32_t total = sum.sleep_s + sum.awake_s;
    if (!sum.valid || total == 0) return est;
    est.hours = total / 3600.0;
    est.sleep_ratio = sum.sleep_s * 100.0 / total;
    est.mv_per_hour = (static_cast<int>(sum.start_mv) - sum.last_mv) / est.hours;
    est.avg_ma = (static_cast<int>(sum.start_pct) - sum.last_pct) *
                 (kBatteryCapacityMah / 100.0) / est.hours;
    est.reliable = est.hours >= kMinEstimateHours;
    return est;
}

void LogRecord(const Record& r) {
    const uint32_t slept = SleptSeconds(r);
    ESP_LOGI(kTag, "Cycle: slept %" PRIu32 " s (%s), wake=%s, awake before sleep %" PRIu32 " s",
             slept, r.pcf_slept_s >= 0 ? "by PCF8563" : "by RC clock only", WakeText(r.wake_cause),
             r.awake_s);
    double drift = 0;
    if (RcDriftPercent(r, drift)) {
        ESP_LOGI(kTag, "Clock: RC %" PRIu32 " s vs PCF8563 %" PRId32 " s -> RC drift %+.2f%% "
                 "(normal: within a few percent)", r.rc_slept_s, r.pcf_slept_s, drift);
    } else {
        ESP_LOGI(kTag, "Clock: RC %" PRIu32 " s, PCF8563 unavailable for comparison", r.rc_slept_s);
    }
    if (r.wake_cause == kWakeTimer && r.planned_s > 0) {
        ESP_LOGI(kTag, "Timer: planned %" PRIu32 " s, actual %" PRIu32 " s (%+" PRId32 " s)",
                 r.planned_s, slept, static_cast<int32_t>(slept) - static_cast<int32_t>(r.planned_s));
    }
    const double hours = slept / 3600.0;
    const int dmv = static_cast<int>(r.mv_before) - r.mv_after;
    ESP_LOGI(kTag, "Battery: %u mV (%u%%) -> %u mV (%u%%), drop %d mV%s",
             r.mv_before, r.pct_before, r.mv_after, r.pct_after, dmv,
             (r.flags & kFlagUsbPower) ? " [USB power present, not a drain sample]" : "");
    if (hours >= 0.5 && !(r.flags & kFlagUsbPower)) {
        ESP_LOGI(kTag, "Battery: %.1f mV/h over this sleep (single cycle, ADC noise ~10 mV)",
                 dmv / hours);
    }
}

void LogSummary(const Summary& sum) {
    if (!sum.valid) {
        ESP_LOGI(kTag, "Estimate: no battery-only cycles yet (unplug USB and let it sleep)");
        return;
    }
    const Estimate est = ComputeEstimate(sum);
    ESP_LOGI(kTag, "Estimate since %s: %.1f h, %" PRIu32 " cycles, asleep %.1f%% of the time",
             FormatLocal(sum.start_epoch).c_str(), est.hours, sum.cycles, est.sleep_ratio);
    ESP_LOGI(kTag, "Estimate: %u mV -> %u mV (%.2f mV/h), %u%% -> %u%%, avg current ~%.2f mA "
             "(%d mAh battery)%s",
             sum.start_mv, sum.last_mv, est.mv_per_hour, sum.start_pct, sum.last_pct, est.avg_ma,
             kBatteryCapacityMah,
             est.reliable ? "" : " [under 12 h, rough only]");
}

void LogPcf(const PcfState& pcf) {
    if (pcf.set_epoch <= 0) {
        ESP_LOGI(kTag, "PCF8563: never written from SNTP yet");
        return;
    }
    if (pcf.last_check_epoch <= 0) {
        ESP_LOGI(kTag, "PCF8563: written at %s, not yet compared against NTP",
                 FormatLocal(pcf.set_epoch).c_str());
        return;
    }
    if (pcf.last_span_s >= kMinPpmSpanSec) {
        ESP_LOGI(kTag, "PCF8563: %+" PRId32 " ms vs NTP after %" PRIu32 " s = %+.1f ppm "
                 "(normal: within +/-50 ppm, about 4 s/day)",
                 pcf.last_offset_ms, pcf.last_span_s,
                 pcf.last_offset_ms * 1000.0 / pcf.last_span_s);
    } else {
        ESP_LOGI(kTag, "PCF8563: %+" PRId32 " ms vs NTP after %" PRIu32 " s (span too short for ppm)",
                 pcf.last_offset_ms, pcf.last_span_s);
    }
}

void PrintHistory(const Ring& ring) {
    if (ring.count == 0) {
        ESP_LOGI(kTag, "History: empty");
        return;
    }
    ESP_LOGI(kTag, "History (oldest first): start | awake | planned | rc_slept | pcf_slept | "
             "rc_drift | wake | mV before->after | %% | usb");
    for (int i = 0; i < ring.count; ++i) {
        const int idx = (ring.head - ring.count + i + kRingSize) % kRingSize;
        const Record& r = ring.records[idx];
        double drift = 0;
        char drift_buf[16];
        if (RcDriftPercent(r, drift)) {
            snprintf(drift_buf, sizeof(drift_buf), "%+.2f%%", drift);
        } else {
            snprintf(drift_buf, sizeof(drift_buf), "n/a");
        }
        ESP_LOGI(kTag, "%2d %s | %5" PRIu32 " | %6" PRIu32 " | %7" PRIu32 " | %7" PRId32
                 " | %8s | %-6s | %4u->%4u | %3u->%3u | %s",
                 i + 1, FormatLocal(r.sleep_start).c_str(), r.awake_s, r.planned_s, r.rc_slept_s,
                 r.pcf_slept_s, drift_buf, WakeText(r.wake_cause), r.mv_before, r.mv_after,
                 r.pct_before, r.pct_after, (r.flags & kFlagUsbPower) ? "yes" : "no");
    }
}

uint8_t CurrentWakeCause() {
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT0)) return kWakeButton;
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) return kWakeTimer;
    return kWakeOther;
}

void RestoreSystemClock(PcfStatus status, int64_t pcf_epoch, int64_t rc_now_ms) {
    if (status != PcfStatus::kOk) {
        ESP_LOGI(kTag, "System clock not restored from PCF8563: %s", PcfStatusText(status));
        return;
    }
    // The chip counts whole seconds, so the true time lies in [pcf, pcf + 1).
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(pcf_epoch);
    tv.tv_usec = 500000;
    settimeofday(&tv, nullptr);
    if (rc_now_ms / 1000 < kMinValidEpoch) {
        ESP_LOGI(kTag, "System clock set from PCF8563: %s (RC clock was unset)",
                 FormatLocal(pcf_epoch).c_str());
    } else {
        ESP_LOGI(kTag, "System clock set from PCF8563: %s (RC clock was %+" PRId64 " ms off)",
                 FormatLocal(pcf_epoch).c_str(), rc_now_ms - (pcf_epoch * 1000 + 500));
    }
}

void SntpSyncTask(void*) {
    auto& board = Board::GetInstance();
    PcfState pcf;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        pcf = LoadPcf();
    }

    time_t first = 0;
    bool valid = false;
    const bool read_ok = board.GetRtcTime(first, valid);
    bool have_offset = false;
    if (read_ok && valid && pcf.set_epoch > 0) {
        // Catch the PCF8563 seconds rollover to measure the offset to ~ms resolution.
        const int64_t deadline = esp_timer_get_time() + kEdgeWaitMs * 1000LL;
        while (esp_timer_get_time() < deadline) {
            time_t now_pcf = 0;
            bool now_valid = false;
            if (!board.GetRtcTime(now_pcf, now_valid)) break;
            if (now_pcf != first) {
                const int64_t edge_ms = NowMs();
                const int64_t offset = static_cast<int64_t>(now_pcf) * 1000 - edge_ms;
                pcf.last_offset_ms = static_cast<int32_t>(
                    offset > INT32_MAX ? INT32_MAX : (offset < INT32_MIN ? INT32_MIN : offset));
                pcf.last_span_s = static_cast<uint32_t>(edge_ms / 1000 - pcf.set_epoch);
                pcf.last_check_epoch = edge_ms / 1000;
                have_offset = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (have_offset) {
            LogPcf(pcf);
        } else {
            ESP_LOGW(kTag, "PCF8563 seconds did not advance within %d ms", kEdgeWaitMs);
        }
    } else if (!read_ok) {
        ESP_LOGW(kTag, "PCF8563 read failed during SNTP check");
    }

    const char* reason = nullptr;
    if (!read_ok || !valid) {
        reason = "chip lost time";
    } else if (pcf.set_epoch <= 0) {
        reason = "first sync";
    } else if (!have_offset) {
        reason = "offset unknown";
    } else if (std::abs(pcf.last_offset_ms) > kPcfResyncThresholdMs) {
        reason = "offset above 2 s";
    }

    if (reason != nullptr) {
        // Write on a second boundary; the chip has no sub-second register.
        struct timeval tv = {};
        gettimeofday(&tv, nullptr);
        const int64_t edge_us = esp_timer_get_time() + (1000000 - tv.tv_usec);
        if (1000000 - tv.tv_usec > 20000) {
            vTaskDelay(pdMS_TO_TICKS((1000000 - tv.tv_usec) / 1000 - 10));
        }
        while (esp_timer_get_time() < edge_us) {
        }
        gettimeofday(&tv, nullptr);
        if (board.SetRtcTime(tv.tv_sec)) {
            pcf.set_epoch = tv.tv_sec;
            pcf.last_check_epoch = 0;
            pcf.last_offset_ms = 0;
            pcf.last_span_s = 0;
            ESP_LOGI(kTag, "PCF8563 written from NTP: %s (%s)",
                     FormatLocal(tv.tv_sec).c_str(), reason);
        } else {
            ESP_LOGW(kTag, "PCF8563 write failed");
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        SaveBlob(kPcfKey, pcf);
    }
    s_sntp_task_running.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

void OnBoot() {
    // SNTP sets the same zone later; set it now so a clock restored from the
    // PCF8563 shows local time before Wi-Fi comes up.
    setenv("TZ", "CST-8", 1);
    tzset();

    auto& board = Board::GetInstance();
    std::lock_guard<std::mutex> lock(s_mutex);
    const PcfState pcf = LoadPcf();
    const int64_t rc_now_ms = NowMs();
    int64_t pcf_now = -1;
    const PcfStatus pcf_status = ReadPcfEpoch(pcf, pcf_now);
    uint16_t mv = 0;
    uint8_t pct = 0;
    bool usb = false;
    board.GetBatteryVoltage(mv, pct, usb);

    Ring ring = LoadRing();
    Summary sum = LoadSummary();
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_DEEPSLEEP && s_snapshot.magic == kSnapshotMagic) {
        Record r = {};
        const bool pcf_both = s_snapshot.pcf_epoch >= 0 && pcf_status == PcfStatus::kOk;
        r.sleep_start = static_cast<uint32_t>(s_snapshot.pcf_epoch >= 0
                                                  ? s_snapshot.pcf_epoch
                                                  : s_snapshot.rc_epoch_ms / 1000);
        const int64_t rc_slept_ms = rc_now_ms - s_snapshot.rc_epoch_ms;
        r.rc_slept_s = rc_slept_ms > 0 ? static_cast<uint32_t>((rc_slept_ms + 500) / 1000) : 0;
        r.pcf_slept_s = pcf_both ? static_cast<int32_t>(pcf_now - s_snapshot.pcf_epoch) : -1;
        r.planned_s = s_snapshot.planned_s;
        r.awake_s = s_snapshot.awake_s;
        r.mv_before = s_snapshot.mv;
        r.mv_after = mv;
        r.pct_before = s_snapshot.pct;
        r.pct_after = pct;
        r.wake_cause = CurrentWakeCause();
        r.flags = static_cast<uint8_t>(((s_snapshot.usb || usb) ? kFlagUsbPower : 0) |
                                       (pcf_both ? kFlagPcfUsed : 0));

        ring.records[ring.head] = r;
        ring.head = (ring.head + 1) % kRingSize;
        if (ring.count < kRingSize) ++ring.count;
        UpdateSummary(sum, r);
        SaveBlob(kRingKey, ring);
        SaveBlob(kSummaryKey, sum);
        LogRecord(r);
        if (r.wake_cause == kWakeTimer && r.pcf_slept_s >= 300 && s_snapshot.programmed_s >= 300) {
            const uint32_t programmed = s_snapshot.programmed_s;
            const uint32_t rc = r.rc_slept_s;
            const uint32_t diff = rc > programmed ? rc - programmed : programmed - rc;
            if (diff <= programmed / 20 && r.pcf_slept_s > 0) {
                UpdateRcScaleLocked(static_cast<float>(rc) / static_cast<float>(r.pcf_slept_s));
            }
        }
    } else {
        ESP_LOGI(kTag, "Boot without a sleep snapshot (reset reason %d); battery %u mV (%u%%), "
                 "usb=%d", static_cast<int>(reset_reason), mv, pct, usb ? 1 : 0);
        if (sum.valid) {
            sum = {};
            SaveBlob(kSummaryKey, sum);
            ESP_LOGI(kTag, "Cumulative estimate restarted: the sleep chain was broken");
        }
    }
    s_snapshot.magic = 0;

    LogSummary(sum);
    LogPcf(pcf);
    PrintHistory(ring);
    RestoreSystemClock(pcf_status, pcf_now, rc_now_ms);
}

void BeforeDeepSleep(uint32_t planned_wake_s, uint32_t programmed_s) {
    auto& board = Board::GetInstance();
    SleepSnapshot snap = {};
    snap.magic = kSnapshotMagic;
    snap.planned_s = planned_wake_s;
    snap.programmed_s = programmed_s;
    snap.awake_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);

    PcfState pcf;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        pcf = LoadPcf();
    }
    snap.rc_epoch_ms = NowMs();
    int64_t pcf_epoch = -1;
    const PcfStatus status = ReadPcfEpoch(pcf, pcf_epoch);
    snap.pcf_epoch = status == PcfStatus::kOk ? pcf_epoch : -1;

    bool usb = false;
    board.GetBatteryVoltage(snap.mv, snap.pct, usb);
    snap.usb = usb ? 1 : 0;
    s_snapshot = snap;

    ESP_LOGI(kTag, "Sleep start: battery %u mV (%u%%) usb=%d, awake %" PRIu32 " s, "
             "planned wake %" PRIu32 " s, RC timer %" PRIu32 " s, clock %s, PCF8563 %s",
             snap.mv, snap.pct, usb ? 1 : 0, snap.awake_s, planned_wake_s, programmed_s,
             FormatLocal(snap.rc_epoch_ms / 1000).c_str(),
             status == PcfStatus::kOk ? FormatLocal(pcf_epoch).c_str() : PcfStatusText(status));
}

float RcSleepScale() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return LoadRcScaleLocked();
}

uint32_t CompensateSleepSec(uint32_t wall_s) {
    if (wall_s == 0) return 0;
    const double rc = static_cast<double>(wall_s) * RcSleepScale() + 0.5;
    if (rc < 1.0) return 1;
    if (rc > static_cast<double>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(rc);
}

void OnSntpSynced() {
    bool expected = false;
    if (!s_sntp_task_running.compare_exchange_strong(expected, true)) return;
    if (xTaskCreate(SntpSyncTask, "pwrdiag_sntp", 4096, nullptr, 3, nullptr) != pdPASS) {
        s_sntp_task_running.store(false);
        ESP_LOGW(kTag, "Failed to start the PCF8563 sync task");
    }
}

std::string HistoryJson() {
    Ring ring;
    Summary sum;
    PcfState pcf;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        ring = LoadRing();
        sum = LoadSummary();
        pcf = LoadPcf();
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* records = cJSON_AddArrayToObject(root, "records");
    for (int i = 0; i < ring.count; ++i) {
        const Record& r = ring.records[(ring.head - ring.count + i + kRingSize) % kRingSize];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "start", FormatLocal(r.sleep_start).c_str());
        cJSON_AddNumberToObject(item, "awake_s", r.awake_s);
        cJSON_AddNumberToObject(item, "planned_s", r.planned_s);
        cJSON_AddNumberToObject(item, "rc_slept_s", r.rc_slept_s);
        cJSON_AddNumberToObject(item, "pcf_slept_s", r.pcf_slept_s);
        double drift = 0;
        if (RcDriftPercent(r, drift)) {
            cJSON_AddNumberToObject(item, "rc_drift_pct", drift);
        } else {
            cJSON_AddNullToObject(item, "rc_drift_pct");
        }
        cJSON_AddStringToObject(item, "wake", WakeText(r.wake_cause));
        cJSON_AddNumberToObject(item, "mv_before", r.mv_before);
        cJSON_AddNumberToObject(item, "mv_after", r.mv_after);
        cJSON_AddNumberToObject(item, "pct_before", r.pct_before);
        cJSON_AddNumberToObject(item, "pct_after", r.pct_after);
        cJSON_AddBoolToObject(item, "usb_power", (r.flags & kFlagUsbPower) != 0);
        cJSON_AddItemToArray(records, item);
    }

    cJSON* summary = cJSON_AddObjectToObject(root, "estimate");
    cJSON_AddBoolToObject(summary, "valid", sum.valid != 0);
    if (sum.valid) {
        const Estimate est = ComputeEstimate(sum);
        cJSON_AddStringToObject(summary, "since", FormatLocal(sum.start_epoch).c_str());
        cJSON_AddNumberToObject(summary, "hours", est.hours);
        cJSON_AddNumberToObject(summary, "cycles", sum.cycles);
        cJSON_AddNumberToObject(summary, "sleep_ratio_pct", est.sleep_ratio);
        cJSON_AddNumberToObject(summary, "start_mv", sum.start_mv);
        cJSON_AddNumberToObject(summary, "last_mv", sum.last_mv);
        cJSON_AddNumberToObject(summary, "mv_per_hour", est.mv_per_hour);
        cJSON_AddNumberToObject(summary, "start_pct", sum.start_pct);
        cJSON_AddNumberToObject(summary, "last_pct", sum.last_pct);
        cJSON_AddNumberToObject(summary, "avg_current_ma", est.avg_ma);
        cJSON_AddBoolToObject(summary, "reliable", est.reliable);
    }

    cJSON_AddNumberToObject(root, "rc_scale", RcSleepScale());

    cJSON* rtc = cJSON_AddObjectToObject(root, "pcf8563");
    cJSON_AddBoolToObject(rtc, "synced", pcf.set_epoch > 0);
    if (pcf.set_epoch > 0) {
        cJSON_AddStringToObject(rtc, "written_at", FormatLocal(pcf.set_epoch).c_str());
    }
    if (pcf.last_check_epoch > 0) {
        cJSON_AddStringToObject(rtc, "checked_at", FormatLocal(pcf.last_check_epoch).c_str());
        cJSON_AddNumberToObject(rtc, "offset_ms", pcf.last_offset_ms);
        cJSON_AddNumberToObject(rtc, "span_s", pcf.last_span_s);
        if (pcf.last_span_s >= kMinPpmSpanSec) {
            cJSON_AddNumberToObject(rtc, "ppm", pcf.last_offset_ms * 1000.0 / pcf.last_span_s);
        }
    }

    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

}  // namespace PowerDiag
}  // namespace ink
