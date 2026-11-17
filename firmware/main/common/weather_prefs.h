/**
 * @file weather_prefs.h
 * @brief Weather announcement preferences in NVS (namespace "weather")
 *
 * Shared by the device settings page and the web /settings endpoint; setters
 * notify the change listener so both views stay in sync.
 */

#ifndef WEATHER_PREFS_H
#define WEATHER_PREFS_H

#include <cstdint>
#include <functional>
#include <string>

namespace ink {

constexpr int kBroadcastOff = -1;
constexpr int kDefaultBroadcastMinute = 7 * 60;

/** @return minute of day (0..1439) for the daily announcement, or kBroadcastOff */
int GetBroadcastMinute();
void SetBroadcastMinute(int minute_of_day);

/** @brief Speak the weather every time the weather page is entered */
bool GetAutoSpeak();
void SetAutoSpeak(bool enabled);

/** @brief Local date (YYYYMMDD) of the last daily announcement */
int GetLastBroadcastDate();
void SetLastBroadcastDate(int yyyymmdd);

/** @return "07:00", or "关闭" for kBroadcastOff */
std::string FormatBroadcastTime(int minute_of_day);
/** @brief Parse "HH:MM"; returns kBroadcastOff on bad input */
int ParseBroadcastTime(const std::string& text);
/** @brief Next value in the device settings cycle: 关闭, 06:00 .. 09:00 every 30 min */
int NextBroadcastMinute(int current);

/** Daytime window for the refresh interval: [06:00, 18:00) */
constexpr int kDayStartMinute = 6 * 60;
constexpr int kDayEndMinute = 18 * 60;
constexpr int kDefaultDayRefreshMin = 60;
constexpr int kDefaultNightRefreshMin = 0;

/** @brief Background weather refresh interval in minutes (0 = off) */
int GetDayRefreshMin();
void SetDayRefreshMin(int minutes);
int GetNightRefreshMin();
void SetNightRefreshMin(int minutes);
/** @brief Next value in the cycle: 关闭, 30, 60, 120, 180, 360 minutes */
int NextRefreshMinutes(int current);
/** @return "关闭", "30分钟", "1小时" ... */
std::string FormatRefreshMinutes(int minutes);
/** @brief Snap to a supported interval; unsupported values fall back to fallback */
int NormalizeRefreshMinutes(int minutes, int fallback);
/** @return the refresh interval (minutes, 0 = off) that applies at minute_of_day */
int RefreshMinAt(int minute_of_day);
/**
 * @brief Interval used to decide whether a cached forecast is still valid
 *
 * Uses the current day or night interval when it is on. When that one is off,
 * uses the other interval if it is on. Returns 0 when both are off, which
 * means the cache is treated as expired and is not fetched automatically.
 */
int CacheCompareIntervalMin(int minute_of_day);
/**
 * @brief Seconds until the next clock-aligned wake for a sleep interval.
 *
 * Sleep a full `interval_min` unless the next alignment mark is closer:
 * interval <= 30 min aligns to :00/:30; larger intervals align to that
 * interval from midnight (60 -> hours, 120 -> even hours, ...).
 * The current mark is skipped so a wake at 07:00 arms 08:00.
 * @return seconds in (0, interval], or -1 if interval_min <= 0
 */
inline int SecondsUntilSteppedAlign(int now_sec_of_day, int interval_min) {
    if (interval_min <= 0) return -1;
    constexpr int kDaySec = 24 * 3600;
    int now = now_sec_of_day % kDaySec;
    if (now < 0) now += kDaySec;
    const int grid_min = interval_min <= 30 ? 30 : interval_min;
    const int grid_sec = grid_min * 60;
    const int interval_sec = interval_min * 60;
    const int next_mark = (now / grid_sec + 1) * grid_sec;
    int delta_mark = next_mark - now;
    if (delta_mark <= 0) delta_mark += kDaySec;
    if (delta_mark < interval_sec) return delta_mark;
    return interval_sec;
}

/**
 * @brief Seconds until the next background refresh is due
 * @param now_sec_of_day current local second of day
 * @param cache_age_sec age of the cached data; pass a huge value when there is none
 * @return 0 if due now, -1 if both intervals are off
 */
int SecondsUntilNextRefresh(int now_sec_of_day, int64_t cache_age_sec);

void SetWeatherPrefsChangedCallback(std::function<void()> callback);

}  // namespace ink

#endif  // WEATHER_PREFS_H
