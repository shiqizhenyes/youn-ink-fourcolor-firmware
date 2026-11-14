/**
 * @file ink_weather_data.h
 * @brief InkSight-style weather model and QWeather v7 parsing (no ESP-IDF dependency)
 *
 * Mirrors InkSight's WEATHER mode content: current conditions, today's range,
 * a 4-day forecast, brief life indices and rule-based advice. Data comes from
 * QWeather's weather/now, weather/7d and indices/1d endpoints.
 */

#ifndef INK_WEATHER_DATA_H
#define INK_WEATHER_DATA_H

#include <cstdint>
#include <string>

namespace ink {

constexpr int kForecastDays = 4;

struct ForecastDay {
    std::string day;   // 今天 / 明天 / 后天 / 周三
    std::string date;  // MM/DD
    std::string desc;
    int code = -1;     // QWeather icon code
    int temp_min = 0;
    int temp_max = 0;
};

struct WeatherData {
    bool valid = false;
    std::string city;
    int temp = 0;
    int code = -1;
    std::string desc;
    int low = 0;
    int high = 0;
    int humidity = -1;       // -1 = unknown
    std::string wind_dir;    // 东北风
    std::string wind_level;  // 3级
    std::string life;        // 穿衣 较冷 · 紫外线 最弱 · 运动 较不宜
    std::string sunrise;     // HH:mm from daily[0]
    std::string sunset;
    std::string advice;
    int forecast_count = 0;
    ForecastDay forecast[kForecastDays];
    int64_t fetched_at = 0;  // unix seconds
};

/** @brief Fixed description for a QWeather icon code, so glyphs stay in the font subset */
const char* QWeatherCodeToDesc(int code);
const char* WindDirFromDegrees(double deg);
std::string GenerateAdvice(const std::string& desc, int low, int high, int humidity, int wind_level);

/**
 * @brief Returns the category if it is a known life-index value, else nullptr
 *
 * Only known values are shown so every glyph is in the generated font subset.
 */
const char* KnownLifeBrief(const char* brief);

/**
 * @brief Parse QWeather v7 weather/now, weather/7d and indices/1d responses
 *
 * now_json and daily_json are required; indices_json may be null or invalid,
 * in which case life is left empty. Day labels come from daily[0].fxDate, so
 * they do not depend on the device clock.
 */
bool ParseQWeather(const char* now_json, const char* daily_json, const char* indices_json,
                   const std::string& city, WeatherData* out);

/** @brief LocationID of the first match in a geo/v2/city/lookup response */
bool ParseQWeatherLocation(const char* json, std::string* id);

/** @brief NVS cache of the city -> LocationID lookup */
std::string SerializeCachedLocation(const std::string& city, const std::string& id);
bool ParseCachedLocation(const std::string& text, const std::string& city, std::string* id);

/**
 * @brief Compact text serialization for NVS caching
 */
std::string SerializeWeather(const WeatherData& data);
bool DeserializeWeather(const std::string& text, WeatherData* out);

/** @brief Spoken Chinese for an integer, e.g. 15 -> 十五, -3 -> 零下三 */
std::string NumberToChinese(int n);

/** @brief Text for the weather announcement; numbers are already spelled out */
std::string BuildWeatherSpeech(const WeatherData& data);

/**
 * @brief Seconds from the given local time until the next minute_of_day
 * @return 1..86400; a time equal to now counts as tomorrow
 */
int SecondsUntilMinuteOfDay(int hour, int minute, int second, int minute_of_day);

}  // namespace ink

#endif  // INK_WEATHER_DATA_H
