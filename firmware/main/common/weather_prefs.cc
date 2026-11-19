/**
 * @file weather_prefs.cc
 * @brief Weather announcement preferences in NVS (namespace "weather")
 */

#include "weather_prefs.h"

#include "settings.h"

#include <cstdio>

namespace ink {

namespace {

constexpr char kNamespace[] = "weather";
constexpr char kBroadcastMinuteKey[] = "bcast_min";
constexpr char kAutoSpeakKey[] = "auto_speak";
constexpr char kWeatherVoiceKey[] = "voice";
constexpr char kLastBroadcastKey[] = "bcast_date";
constexpr char kDayRefreshKey[] = "day_refresh";
constexpr char kNightRefreshKey[] = "night_refresh";
constexpr int kMinutesPerDay = 24 * 60;
constexpr int kCycle[] = {kBroadcastOff, 360, 390, 420, 450, 480, 510, 540};
constexpr int kRefreshCycle[] = {0, 30, 60, 120, 180, 360};
constexpr int kRefreshCycleCount = sizeof(kRefreshCycle) / sizeof(kRefreshCycle[0]);

int NormalizeWeatherVoice(int voice) {
    return voice == kWeatherVoiceFemaleA ? kWeatherVoiceFemaleA : kWeatherVoiceXiaole;
}

std::function<void()>& ChangedCallback() {
    static std::function<void()> callback;
    return callback;
}

void NotifyChanged() {
    if (ChangedCallback()) ChangedCallback()();
}

}  // namespace

int GetBroadcastMinute() {
    Settings nvs(kNamespace);
    const int minute = nvs.GetInt(kBroadcastMinuteKey, kDefaultBroadcastMinute);
    return minute >= 0 && minute < kMinutesPerDay ? minute : kBroadcastOff;
}

void SetBroadcastMinute(int minute_of_day) {
    if (minute_of_day < 0 || minute_of_day >= kMinutesPerDay) minute_of_day = kBroadcastOff;
    {
        Settings nvs(kNamespace, true);
        nvs.SetInt(kBroadcastMinuteKey, minute_of_day);
    }
    NotifyChanged();
}

bool GetAutoSpeak() {
    Settings nvs(kNamespace);
    return nvs.GetBool(kAutoSpeakKey, true);
}

void SetAutoSpeak(bool enabled) {
    {
        Settings nvs(kNamespace, true);
        nvs.SetBool(kAutoSpeakKey, enabled);
    }
    NotifyChanged();
}

int GetWeatherVoice() {
    Settings nvs(kNamespace);
    return NormalizeWeatherVoice(nvs.GetInt(kWeatherVoiceKey, kWeatherVoiceXiaole));
}

void SetWeatherVoice(int voice) {
    {
        Settings nvs(kNamespace, true);
        nvs.SetInt(kWeatherVoiceKey, NormalizeWeatherVoice(voice));
    }
    NotifyChanged();
}

int NextWeatherVoice(int current) {
    return NormalizeWeatherVoice(current) == kWeatherVoiceXiaole ? kWeatherVoiceFemaleA
                                                                 : kWeatherVoiceXiaole;
}

std::string FormatWeatherVoice(int voice) {
    return NormalizeWeatherVoice(voice) == kWeatherVoiceFemaleA ? "女声A" : "小乐";
}

const char* WeatherVoiceToken(int voice) {
    return NormalizeWeatherVoice(voice) == kWeatherVoiceFemaleA ? "female_a" : "xiaole";
}

int ParseWeatherVoice(const std::string& text) {
    return text == "female_a" ? kWeatherVoiceFemaleA : kWeatherVoiceXiaole;
}

int GetLastBroadcastDate() {
    Settings nvs(kNamespace);
    return nvs.GetInt(kLastBroadcastKey, 0);
}

void SetLastBroadcastDate(int yyyymmdd) {
    Settings nvs(kNamespace, true);
    nvs.SetInt(kLastBroadcastKey, yyyymmdd);
}

std::string FormatBroadcastTime(int minute_of_day) {
    if (minute_of_day < 0 || minute_of_day >= kMinutesPerDay) return "关闭";
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", minute_of_day / 60, minute_of_day % 60);
    return buf;
}

int ParseBroadcastTime(const std::string& text) {
    int h = -1;
    int m = -1;
    char tail = 0;
    if (sscanf(text.c_str(), "%d:%d%c", &h, &m, &tail) != 2) return kBroadcastOff;
    if (h < 0 || h > 23 || m < 0 || m > 59) return kBroadcastOff;
    return h * 60 + m;
}

int NextBroadcastMinute(int current) {
    constexpr int kCount = sizeof(kCycle) / sizeof(kCycle[0]);
    for (int i = 0; i < kCount; ++i) {
        if (kCycle[i] == current) return kCycle[(i + 1) % kCount];
    }
    // A custom time from the web page: continue with the next preset after it.
    for (int i = 1; i < kCount; ++i) {
        if (kCycle[i] > current) return kCycle[i];
    }
    return kBroadcastOff;
}

int NormalizeRefreshMinutes(int minutes, int fallback) {
    for (int option : kRefreshCycle) {
        if (option == minutes) return minutes;
    }
    return fallback;
}

int GetDayRefreshMin() {
    Settings nvs(kNamespace);
    return NormalizeRefreshMinutes(nvs.GetInt(kDayRefreshKey, kDefaultDayRefreshMin),
                                   kDefaultDayRefreshMin);
}

void SetDayRefreshMin(int minutes) {
    {
        Settings nvs(kNamespace, true);
        nvs.SetInt(kDayRefreshKey, NormalizeRefreshMinutes(minutes, kDefaultDayRefreshMin));
    }
    NotifyChanged();
}

int GetNightRefreshMin() {
    Settings nvs(kNamespace);
    return NormalizeRefreshMinutes(nvs.GetInt(kNightRefreshKey, kDefaultNightRefreshMin),
                                   kDefaultNightRefreshMin);
}

void SetNightRefreshMin(int minutes) {
    {
        Settings nvs(kNamespace, true);
        nvs.SetInt(kNightRefreshKey, NormalizeRefreshMinutes(minutes, kDefaultNightRefreshMin));
    }
    NotifyChanged();
}

int NextRefreshMinutes(int current) {
    for (int i = 0; i < kRefreshCycleCount; ++i) {
        if (kRefreshCycle[i] == current) return kRefreshCycle[(i + 1) % kRefreshCycleCount];
    }
    return kRefreshCycle[0];
}

std::string FormatRefreshMinutes(int minutes) {
    if (minutes <= 0) return "关闭";
    char buf[32];
    if (minutes % 60 == 0) {
        snprintf(buf, sizeof(buf), "%d小时", minutes / 60);
    } else {
        snprintf(buf, sizeof(buf), "%d分钟", minutes);
    }
    return buf;
}

int RefreshMinAt(int minute_of_day) {
    const bool day = minute_of_day >= kDayStartMinute && minute_of_day < kDayEndMinute;
    return day ? GetDayRefreshMin() : GetNightRefreshMin();
}

int CacheCompareIntervalMin(int minute_of_day) {
    const bool day = minute_of_day >= kDayStartMinute && minute_of_day < kDayEndMinute;
    const int current = day ? GetDayRefreshMin() : GetNightRefreshMin();
    if (current > 0) return current;
    const int other = day ? GetNightRefreshMin() : GetDayRefreshMin();
    return other > 0 ? other : 0;
}

namespace {

int SecondsUntilNextOnWindow(int now_sec, int day_min, int night_min) {
    constexpr int kDaySec = 24 * 3600;
    constexpr int kDayStartSec = kDayStartMinute * 60;
    constexpr int kDayEndSec = kDayEndMinute * 60;
    int best = -1;
    auto consider = [&](int target_sec) {
        int delta = target_sec - now_sec;
        if (delta <= 0) delta += kDaySec;
        if (best < 0 || delta < best) best = delta;
    };
    if (day_min > 0) consider(kDayStartSec);
    if (night_min > 0) consider(kDayEndSec);
    return best;
}

}  // namespace

int SecondsUntilNextRefresh(int now_sec_of_day, int64_t cache_age_sec) {
    const int day_min = GetDayRefreshMin();
    const int night_min = GetNightRefreshMin();
    if (day_min <= 0 && night_min <= 0) return -1;

    constexpr int kDaySec = 24 * 3600;
    int now = now_sec_of_day % kDaySec;
    if (now < 0) now += kDaySec;
    const int interval = RefreshMinAt(now / 60);
    if (interval > 0) {
        const int grid = interval <= 30 ? 30 : interval;
        if ((now / 60) % grid == 0 &&
            cache_age_sec >= static_cast<int64_t>(interval) * 60) {
            return 0;
        }
        const int step = SecondsUntilSteppedAlign(now, interval);
        const int land_min = ((now + step) % kDaySec) / 60;
        if (RefreshMinAt(land_min) > 0) return step;
    }
    return SecondsUntilNextOnWindow(now, day_min, night_min);
}

void SetWeatherPrefsChangedCallback(std::function<void()> callback) {
    ChangedCallback() = std::move(callback);
}

}  // namespace ink
