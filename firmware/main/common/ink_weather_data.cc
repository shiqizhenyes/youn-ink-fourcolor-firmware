/**
 * @file ink_weather_data.cc
 * @brief InkSight-style weather model and QWeather v7 parsing
 */

#include "ink_weather_data.h"

#include <cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ink {

namespace {

constexpr int kCacheVersion = 4;

const char* const kWeekdayShort[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};

// Every category the shown indices (sport, dressing, UV) are known to return.
const char* const kLifeBriefs[] = {
    "炎热", "热", "较热", "舒适", "较舒适", "凉", "较凉", "较冷", "冷", "寒冷",
    "最弱", "弱", "中等", "强", "很强",
    "适宜", "较适宜", "一般", "较不宜", "不宜",
    "少发", "较易发", "易发", "极易发",
};

struct CodeDesc {
    int code;
    const char* desc;
};

// QWeather icon codes; night variants (15x, 35x, 45x) share the day wording.
const CodeDesc kCodeDescs[] = {
    {100, "晴"}, {101, "多云"}, {102, "少云"}, {103, "晴间多云"}, {104, "阴"},
    {150, "晴"}, {151, "多云"}, {152, "少云"}, {153, "晴间多云"},
    {300, "阵雨"}, {301, "强阵雨"}, {302, "雷阵雨"}, {303, "强雷阵雨"}, {304, "雷阵雨伴有冰雹"},
    {305, "小雨"}, {306, "中雨"}, {307, "大雨"}, {308, "极端降雨"}, {309, "毛毛雨"},
    {310, "暴雨"}, {311, "大暴雨"}, {312, "特大暴雨"}, {313, "冻雨"}, {314, "小到中雨"},
    {315, "中到大雨"}, {316, "大到暴雨"}, {317, "暴雨到大暴雨"}, {318, "大暴雨到特大暴雨"},
    {350, "阵雨"}, {351, "强阵雨"}, {399, "雨"},
    {400, "小雪"}, {401, "中雪"}, {402, "大雪"}, {403, "暴雪"}, {404, "雨夹雪"},
    {405, "雨雪天气"}, {406, "阵雨夹雪"}, {407, "阵雪"}, {408, "小到中雪"}, {409, "中到大雪"},
    {410, "大到暴雪"}, {456, "阵雨夹雪"}, {457, "阵雪"}, {499, "雪"},
    {500, "薄雾"}, {501, "雾"}, {502, "霾"}, {503, "扬沙"}, {504, "浮尘"}, {507, "沙尘暴"},
    {508, "强沙尘暴"}, {509, "浓雾"}, {510, "强浓雾"}, {511, "中度霾"}, {512, "重度霾"},
    {513, "严重霾"}, {514, "大雾"}, {515, "特强浓雾"},
    {900, "热"}, {901, "冷"},
};

// Days since 1970-01-01 for a proleptic Gregorian date.
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// 0 = Monday .. 6 = Sunday
int WeekdayMondayFirst(int y, int m, int d) {
    const int64_t days = DaysFromCivil(y, m, d);
    return static_cast<int>(((days % 7) + 7 + 3) % 7);
}

bool ParseIsoDate(const char* s, int* y, int* m, int* d) {
    return s && sscanf(s, "%d-%d-%d", y, m, d) == 3;
}

// QWeather returns numbers as strings ("16"); accept real numbers too.
bool NumberField(const cJSON* obj, const char* key, double* out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        char* end = nullptr;
        const double v = strtod(item->valuestring, &end);
        if (end != item->valuestring) {
            *out = v;
            return true;
        }
    }
    return false;
}

int IntField(const cJSON* obj, const char* key, int fallback) {
    double v = 0;
    return NumberField(obj, key, &v) ? static_cast<int>(std::lround(v)) : fallback;
}

std::string StringField(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

// v7 responses carry their own status in "code"; anything but "200" is an error.
bool IsOkResponse(const cJSON* root) {
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0;
}

std::string BuildLife(const cJSON* indices) {
    struct Index {
        const char* type;
        const char* label;
    };
    static const Index kShown[] = {{"3", "穿衣"}, {"5", "紫外线"}, {"1", "运动"}};
    std::string life;
    for (const auto& idx : kShown) {
        const cJSON* item = nullptr;
        const char* known = nullptr;
        cJSON_ArrayForEach(item, indices) {
            const cJSON* type = cJSON_GetObjectItemCaseSensitive(item, "type");
            if (!cJSON_IsString(type) || strcmp(type->valuestring, idx.type) != 0) continue;
            const cJSON* category = cJSON_GetObjectItemCaseSensitive(item, "category");
            known = cJSON_IsString(category) ? KnownLifeBrief(category->valuestring) : nullptr;
            break;
        }
        if (!known) continue;
        if (!life.empty()) life += " · ";
        life += idx.label;
        life += ' ';
        life += known;
    }
    return life;
}

std::string DegreesSpeech(int value) {
    return NumberToChinese(value) + "度";
}

}  // namespace

const char* QWeatherCodeToDesc(int code) {
    for (const auto& entry : kCodeDescs) {
        if (entry.code == code) return entry.desc;
    }
    return "未知";
}

const char* WindDirFromDegrees(double deg) {
    static const char* const kDirs[] = {"北风", "东北风", "东风", "东南风",
                                        "南风", "西南风", "西风", "西北风"};
    double normalized = std::fmod(deg, 360.0);
    if (normalized < 0) normalized += 360.0;
    const int idx = static_cast<int>(normalized / 45.0 + 0.5) % 8;
    return kDirs[idx];
}

std::string GenerateAdvice(const std::string& desc, int low, int high, int humidity, int wind_level) {
    auto has = [&desc](const char* word) { return desc.find(word) != std::string::npos; };
    if (has("雷")) return "有雷雨，尽量减少外出";
    if (has("雪")) return "有雪天冷，注意保暖防滑";
    if (has("雨")) return "有雨记得带伞，注意路滑";
    if (has("雾")) return "有雾能见度低，出行留意";
    if (high >= 32) return "天气炎热，注意防晒补水";
    if (low <= 5) return "气温较低，外出注意保暖";
    if (high - low >= 8) return "早晚温差大，记得带外套";
    if (wind_level >= 5) return "风力较大，出门注意防风";
    if (humidity >= 85) return "空气潮湿，注意防潮添衣";
    if (high >= 26) return "气温偏高，穿着轻薄透气";
    return "气温适宜，轻装出行";
}

const char* KnownLifeBrief(const char* brief) {
    if (!brief) return nullptr;
    for (const char* known : kLifeBriefs) {
        if (strcmp(brief, known) == 0) return known;
    }
    return nullptr;
}

bool ParseQWeather(const char* now_json, const char* daily_json, const char* indices_json,
                   const std::string& city, WeatherData* out) {
    if (!now_json || !daily_json || !out) return false;
    cJSON* now_root = cJSON_Parse(now_json);
    cJSON* daily_root = cJSON_Parse(daily_json);
    cJSON* indices_root = indices_json ? cJSON_Parse(indices_json) : nullptr;
    auto cleanup = [&]() {
        cJSON_Delete(now_root);
        cJSON_Delete(daily_root);
        cJSON_Delete(indices_root);
    };

    const cJSON* now = now_root && IsOkResponse(now_root)
                           ? cJSON_GetObjectItemCaseSensitive(now_root, "now")
                           : nullptr;
    const cJSON* daily = daily_root && IsOkResponse(daily_root)
                             ? cJSON_GetObjectItemCaseSensitive(daily_root, "daily")
                             : nullptr;
    const cJSON* today = cJSON_IsArray(daily) ? cJSON_GetArrayItem(daily, 0) : nullptr;
    const cJSON* today_date = today ? cJSON_GetObjectItemCaseSensitive(today, "fxDate") : nullptr;
    int ty = 0, tm = 0, td = 0;
    if (!cJSON_IsObject(now) || !cJSON_IsObject(today) || !cJSON_IsString(today_date) ||
        !ParseIsoDate(today_date->valuestring, &ty, &tm, &td)) {
        cleanup();
        return false;
    }

    WeatherData data;
    data.city = city;
    data.low = IntField(today, "tempMin", 0);
    data.high = IntField(today, "tempMax", 0);
    data.temp = IntField(now, "temp", data.high);
    data.code = IntField(now, "icon", IntField(today, "iconDay", -1));
    data.desc = QWeatherCodeToDesc(data.code);
    data.humidity = IntField(now, "humidity", IntField(today, "humidity", -1));
    data.sunrise = StringField(today, "sunrise");
    data.sunset = StringField(today, "sunset");

    double degree = 0;
    if (NumberField(now, "wind360", &degree)) {
        data.wind_dir = WindDirFromDegrees(degree);
    }
    const int wind_level = IntField(now, "windScale", 0);
    if (wind_level > 0) {
        data.wind_level = std::to_string(wind_level) + "级";
    }

    const cJSON* indices = indices_root && IsOkResponse(indices_root)
                               ? cJSON_GetObjectItemCaseSensitive(indices_root, "daily")
                               : nullptr;
    if (cJSON_IsArray(indices)) {
        data.life = BuildLife(indices);
    }

    data.advice = GenerateAdvice(data.desc, data.low, data.high,
                                 data.humidity < 0 ? 0 : data.humidity, wind_level);

    const int64_t today_days = DaysFromCivil(ty, tm, td);
    const cJSON* day_item = nullptr;
    cJSON_ArrayForEach(day_item, daily) {
        if (data.forecast_count >= kForecastDays) break;
        const cJSON* date_item = cJSON_GetObjectItemCaseSensitive(day_item, "fxDate");
        int y = 0, m = 0, d = 0;
        if (!cJSON_IsString(date_item) || !ParseIsoDate(date_item->valuestring, &y, &m, &d)) continue;

        ForecastDay& fd = data.forecast[data.forecast_count];
        const int64_t delta = DaysFromCivil(y, m, d) - today_days;
        if (delta == 0) {
            fd.day = "今天";
        } else if (delta == 1) {
            fd.day = "明天";
        } else if (delta == 2) {
            fd.day = "后天";
        } else {
            fd.day = kWeekdayShort[WeekdayMondayFirst(y, m, d)];
        }
        char date_buf[16];
        snprintf(date_buf, sizeof(date_buf), "%02d/%02d", m, d);
        fd.date = date_buf;
        fd.code = IntField(day_item, "iconDay", -1);
        fd.desc = QWeatherCodeToDesc(fd.code);
        fd.temp_min = IntField(day_item, "tempMin", 0);
        fd.temp_max = IntField(day_item, "tempMax", 0);
        data.forecast_count++;
    }

    cleanup();
    data.valid = true;
    *out = data;
    return true;
}

bool ParseQWeatherLocation(const char* json, std::string* id) {
    if (!json || !id) return false;
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON* list = IsOkResponse(root) ? cJSON_GetObjectItemCaseSensitive(root, "location") : nullptr;
    const cJSON* first = cJSON_IsArray(list) ? cJSON_GetArrayItem(list, 0) : nullptr;
    const cJSON* id_item = first ? cJSON_GetObjectItemCaseSensitive(first, "id") : nullptr;
    const bool ok = cJSON_IsString(id_item) && id_item->valuestring[0] != '\0';
    if (ok) *id = id_item->valuestring;
    cJSON_Delete(root);
    return ok;
}

std::string SerializeCachedLocation(const std::string& city, const std::string& id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "city", city.c_str());
    cJSON_AddStringToObject(root, "id", id.c_str());
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

bool ParseCachedLocation(const std::string& text, const std::string& city, std::string* id) {
    if (text.empty() || !id) return false;
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) return false;
    const cJSON* city_item = cJSON_GetObjectItemCaseSensitive(root, "city");
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    const bool ok = cJSON_IsString(city_item) && city == city_item->valuestring &&
                    cJSON_IsString(id_item) && id_item->valuestring[0] != '\0';
    if (ok) *id = id_item->valuestring;
    cJSON_Delete(root);
    return ok;
}

std::string SerializeWeather(const WeatherData& data) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kCacheVersion);
    cJSON_AddStringToObject(root, "city", data.city.c_str());
    cJSON_AddNumberToObject(root, "temp", data.temp);
    cJSON_AddNumberToObject(root, "code", data.code);
    cJSON_AddNumberToObject(root, "low", data.low);
    cJSON_AddNumberToObject(root, "high", data.high);
    cJSON_AddNumberToObject(root, "hum", data.humidity);
    cJSON_AddStringToObject(root, "wd", data.wind_dir.c_str());
    cJSON_AddStringToObject(root, "wl", data.wind_level.c_str());
    cJSON_AddStringToObject(root, "life", data.life.c_str());
    cJSON_AddStringToObject(root, "sr", data.sunrise.c_str());
    cJSON_AddStringToObject(root, "ss", data.sunset.c_str());
    cJSON_AddStringToObject(root, "adv", data.advice.c_str());
    cJSON_AddNumberToObject(root, "at", static_cast<double>(data.fetched_at));
    cJSON* days = cJSON_AddArrayToObject(root, "fc");
    for (int i = 0; i < data.forecast_count; ++i) {
        const ForecastDay& fd = data.forecast[i];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "d", fd.day.c_str());
        cJSON_AddStringToObject(item, "t", fd.date.c_str());
        cJSON_AddNumberToObject(item, "c", fd.code);
        cJSON_AddNumberToObject(item, "lo", fd.temp_min);
        cJSON_AddNumberToObject(item, "hi", fd.temp_max);
        cJSON_AddItemToArray(days, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

bool DeserializeWeather(const std::string& text, WeatherData* out) {
    if (!out || text.empty()) return false;
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) return false;

    auto str = [](const cJSON* obj, const char* key) -> std::string {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
        return cJSON_IsString(item) ? item->valuestring : "";
    };
    auto num = [](const cJSON* obj, const char* key, double fallback) -> double {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
        return cJSON_IsNumber(item) ? item->valuedouble : fallback;
    };

    // Older caches use Open-Meteo (WMO) or Seniverse codes; drop them.
    if (static_cast<int>(num(root, "v", 0)) != kCacheVersion) {
        cJSON_Delete(root);
        return false;
    }

    WeatherData data;
    data.city = str(root, "city");
    data.temp = static_cast<int>(num(root, "temp", 0));
    data.code = static_cast<int>(num(root, "code", -1));
    data.desc = QWeatherCodeToDesc(data.code);
    data.low = static_cast<int>(num(root, "low", 0));
    data.high = static_cast<int>(num(root, "high", 0));
    data.humidity = static_cast<int>(num(root, "hum", -1));
    data.wind_dir = str(root, "wd");
    data.wind_level = str(root, "wl");
    data.life = str(root, "life");
    data.sunrise = str(root, "sr");
    data.sunset = str(root, "ss");
    data.advice = str(root, "adv");
    data.fetched_at = static_cast<int64_t>(num(root, "at", 0));

    const cJSON* days = cJSON_GetObjectItemCaseSensitive(root, "fc");
    if (cJSON_IsArray(days)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, days) {
            if (data.forecast_count >= kForecastDays) break;
            ForecastDay& fd = data.forecast[data.forecast_count++];
            fd.day = str(item, "d");
            fd.date = str(item, "t");
            fd.code = static_cast<int>(num(item, "c", -1));
            fd.desc = QWeatherCodeToDesc(fd.code);
            fd.temp_min = static_cast<int>(num(item, "lo", 0));
            fd.temp_max = static_cast<int>(num(item, "hi", 0));
        }
    }

    cJSON_Delete(root);
    data.valid = true;
    *out = data;
    return true;
}

std::string NumberToChinese(int n) {
    static const char* const kDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (n < 0) return "零下" + NumberToChinese(-n);
    if (n < 10) return kDigits[n];
    if (n < 20) return std::string("十") + (n % 10 ? kDigits[n % 10] : "");
    if (n < 100) return std::string(kDigits[n / 10]) + "十" + (n % 10 ? kDigits[n % 10] : "");
    if (n < 1000) {
        const int rest = n % 100;
        std::string s = std::string(kDigits[n / 100]) + "百";
        if (rest == 0) return s;
        if (rest < 10) return s + "零" + kDigits[rest];
        // 110 is 一百一十, not 一百十.
        return s + (rest < 20 ? "一" : "") + NumberToChinese(rest);
    }
    return std::to_string(n);
}

std::string BuildWeatherSpeech(const WeatherData& data) {
    if (!data.valid) return "";
    std::string s = data.city + "现在" + data.desc + "，" + DegreesSpeech(data.temp) + "。";
    s += "今天最低" + DegreesSpeech(data.low) + "，最高" + DegreesSpeech(data.high);
    if (!data.wind_dir.empty()) {
        s += "，" + data.wind_dir;
        const int level = atoi(data.wind_level.c_str());
        if (level > 0) s += NumberToChinese(level) + "级";
    }
    if (data.humidity >= 0) s += "，湿度百分之" + NumberToChinese(data.humidity);
    s += "。";

    if (!data.life.empty()) {
        // "穿衣 较冷 · 紫外线 最弱" -> "穿衣较冷，紫外线最弱"
        std::string life;
        const std::string sep = " · ";
        for (size_t i = 0; i < data.life.size();) {
            if (data.life.compare(i, sep.size(), sep) == 0) {
                life += "，";
                i += sep.size();
            } else if (data.life[i] == ' ') {
                ++i;
            } else {
                life += data.life[i++];
            }
        }
        s += life + "。";
    }
    if (!data.advice.empty()) s += data.advice + "。";

    for (int i = 0; i < data.forecast_count; ++i) {
        const ForecastDay& fd = data.forecast[i];
        if (fd.day != "明天") continue;
        s += "明天" + fd.desc + "，" + NumberToChinese(fd.temp_min) + "到" +
             DegreesSpeech(fd.temp_max) + "。";
        break;
    }
    return s;
}

int SecondsUntilMinuteOfDay(int hour, int minute, int second, int minute_of_day) {
    constexpr int kDay = 24 * 60 * 60;
    int delta = minute_of_day * 60 - (hour * 3600 + minute * 60 + second);
    while (delta <= 0) delta += kDay;
    return delta;
}

}  // namespace ink
