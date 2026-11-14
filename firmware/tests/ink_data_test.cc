// Host regression test for the InkSight weather/calendar port (QWeather data); see README.md.
// --dump-terms prints every table solar term as "Y-M-D name" (diff against
// InkSight backend/core/config.py); --dump-lunar prints "Y-M-D ly lm ld leap"
// for 1999-2100 (diff against zhdate, which InkSight uses).
#include "gzip_util.h"
#include "ink_calendar_data.h"
#include "ink_weather_data.h"
#ifdef INK_TEST_JWT
#include "qweather_jwt.h"
#endif

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

namespace {

// Captured from QWeather v7 (杭州 101210101, 2026-11-07).
const char kNowJson[] = R"({"code":"200","updateTime":"2026-11-07T21:47+08:00","fxLink":"https://www.qweather.com/weather/hangzhou-101210101.html","now":{"obsTime":"2026-11-07T21:37+08:00","temp":"15","icon":"305","text":"小雨","wind360":"90","windDir":"东风","windScale":"3","windSpeed":"17","humidity":"91","precip":"3.1","pressure":"1024","cloud":"10","dew":"27","feelsLike":"12","vis":"6"},"refer":{"sources":["QWeather"],"license":["QWeather Developers License"]}})";

const char kDailyJson[] = R"({"code":"200","updateTime":"2026-11-07T21:47+08:00","fxLink":"https://www.qweather.com/weather/hangzhou-101210101.html","daily":[{"fxDate":"2026-11-07","sunrise":"06:17","sunset":"17:07","moonrise":"04:11","moonset":"15:38","moonPhase":"残月","moonPhaseIcon":"807","tempMax":"19","tempMin":"14","iconDay":"305","textDay":"小雨","iconNight":"306","textNight":"中雨","wind360Day":"45","windDirDay":"东北风","windScaleDay":"1-3","windSpeedDay":"3","wind360Night":"45","windDirNight":"东北风","windScaleNight":"1-3","windSpeedNight":"3","humidity":"93","precip":"1.7","pressure":"1022","uvIndex":"1","vis":"22","cloud":"60"},{"fxDate":"2026-11-08","sunrise":"06:18","sunset":"17:06","moonrise":"05:10","moonset":"16:10","moonPhase":"残月","moonPhaseIcon":"807","tempMax":"17","tempMin":"12","iconDay":"305","textDay":"小雨","iconNight":"305","textNight":"小雨","wind360Day":"225","windDirDay":"西南风","windScaleDay":"1-3","windSpeedDay":"3","wind360Night":"225","windDirNight":"西南风","windScaleNight":"1-3","windSpeedNight":"3","humidity":"84","precip":"4.5","pressure":"1031","uvIndex":"1","vis":"23","cloud":"91"},{"fxDate":"2026-11-09","sunrise":"06:19","sunset":"17:06","moonrise":"06:09","moonset":"16:45","moonPhase":"新月","moonPhaseIcon":"800","tempMax":"14","tempMin":"8","iconDay":"101","textDay":"多云","iconNight":"150","textNight":"晴","wind360Day":"225","windDirDay":"西南风","windScaleDay":"1-3","windSpeedDay":"3","wind360Night":"225","windDirNight":"西南风","windScaleNight":"1-3","windSpeedNight":"3","humidity":"65","precip":"0.0","pressure":"1030","uvIndex":"3","vis":"24","cloud":"4"},{"fxDate":"2026-11-10","sunrise":"06:20","sunset":"17:05","moonrise":"07:09","moonset":"17:25","moonPhase":"蛾眉月","moonPhaseIcon":"801","tempMax":"16","tempMin":"8","iconDay":"100","textDay":"晴","iconNight":"150","textNight":"晴","wind360Day":"225","windDirDay":"西南风","windScaleDay":"1-3","windSpeedDay":"3","wind360Night":"225","windDirNight":"西南风","windScaleNight":"1-3","windSpeedNight":"3","humidity":"76","precip":"0.0","pressure":"1025","uvIndex":"4","vis":"24","cloud":"0"},{"fxDate":"2026-11-11","sunrise":"06:21","sunset":"17:05","moonrise":"08:07","moonset":"18:10","moonPhase":"蛾眉月","moonPhaseIcon":"801","tempMax":"18","tempMin":"11","iconDay":"100","textDay":"晴","iconNight":"151","textNight":"多云","wind360Day":"45","windDirDay":"东北风","windScaleDay":"1-3","windSpeedDay":"3","wind360Night":"225","windDirNight":"西南风","windScaleNight":"1-3","windSpeedNight":"3","humidity":"79","precip":"0.0","pressure":"1021","uvIndex":"4","vis":"24","cloud":"1"},{"fxDate":"2026-11-12","sunrise":"06:21","sunset":"17:04","moonrise":"09:02","moonset":"19:00","moonPhase":"蛾眉月","moonPhaseIcon":"801","tempMax":"20","tempMin":"14","iconDay":"101","textDay":"多云","iconNight":"305","textNight":"小雨","wind360Day":"45","windDirDay":"东北风","windScaleDay":"1-3","windSpeedDay":"3","wind360Night":"225","windDirNight":"西南风","windScaleNight":"1-3","windSpeedNight":"3","humidity":"74","precip":"0.0","pressure":"1021","uvIndex":"2","vis":"24","cloud":"22"},{"fxDate":"2026-11-13","sunrise":"06:22","sunset":"17:03","moonrise":"09:53","moonset":"19:53","moonPhase":"蛾眉月","moonPhaseIcon":"801","tempMax":"19","tempMin":"14","iconDay":"305","textDay":"小雨","iconNight":"305","textNight":"小雨","wind360Day":"45","windDirDay":"东北风","windScaleDay":"1-3","windSpeedDay":"3","wind360Night":"225","windDirNight":"西南风","windScaleNight":"1-3","windSpeedNight":"3","humidity":"90","precip":"0.5","pressure":"1017","uvIndex":"1","vis":"24","cloud":"55"}],"refer":{"sources":["QWeather"],"license":["QWeather Developers License"]}})";

const char kIndicesJson[] = R"({"code":"200","updateTime":"2026-11-07T21:51+08:00","fxLink":"https://www.qweather.com/indices/hangzhou-101210101.html","daily":[{"date":"2026-11-07","type":"1","name":"运动指数","level":"3","category":"较不宜","text":"有降水，若坚持户外运动，须注意携带雨具并注意避雨防滑，推荐您在室内进行健身休闲运动。"},{"date":"2026-11-07","type":"3","name":"穿衣指数","level":"3","category":"较冷","text":"建议着厚外套加毛衣等服装。年老体弱者宜着大衣、呢外套加羊毛衫。"},{"date":"2026-11-07","type":"5","name":"紫外线指数","level":"1","category":"最弱","text":"属弱紫外线辐射天气，无需特别防护。若长期在户外，建议涂擦SPF在8-12之间的防晒护肤品。"}],"refer":{"sources":["QWeather"],"license":["QWeather Developers License"]}})";

const char kGeoJson[] = R"({"code":"200","location":[{"name":"杭州","id":"101210101","lat":"30.24603","lon":"120.21079","adm2":"杭州","adm1":"浙江省","country":"中国","tz":"Asia/Shanghai","utcOffset":"+08:00","isDst":"0","type":"city","rank":"11","fxLink":"https://www.qweather.com/weather/hangzhou-101210101.html"}],"refer":{"sources":["QWeather"],"license":["QWeather Developers License"]}})";

// The raw (gzip) body of the weather/now request above, as the server sent it.
const char kNowGzip[] =
    "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff\x6d\x90\xb1\x4e\xc3\x30\x10\x86\x5f\xa5\xf2\x4a\x9a\xd8"
"\x49\xdb\x40\xe6\x8e\x5d\x10\x95\x18\x50\x87\xe0\x5c\xb0\xd5\x24\x0e\xb6\x53\x17\xaa\xbe\x02\x3c"
"\x00\x6c\x2c\x8c\x8c\x3c\x10\x0c\x7d\x8b\x9e\xd3\x20\x18\x18\x2c\xdd\xff\xdd\xdd\xff\xeb\xbc\x23"
"\x5c\x15\x40\x32\x12\x53\x4a\x02\xd2\xb5\x45\x6e\x61\x29\xeb\x13\x8a\x67\x63\xc6\xc6\x34\x5d\xc6"
"\x2c\x9b\xa4\x67\xf4\x3c\xeb\xa7\xca\xed\x42\x36\x6b\x9c\x10\xd6\xb6\x26\x8b\x22\xe7\x5c\x78\xef"
"\x20\xb7\x02\x74\xc8\x55\x1d\x0d\x75\x24\xf2\xe6\xee\x51\xa8\x6e\xcc\x28\x8b\xf1\x51\x16\x0a\x5b"
"\x57\xe8\xd1\x28\x47\xb2\x1d\x51\xb7\xe6\xdf\xb4\xe4\x37\xcd\x42\xdd\x62\x9f\x4d\xb1\x96\x5c\x35"
"\x58\x27\x74\xda\x37\xb6\x16\xc5\xf7\xc7\xf3\xe1\xe5\x1d\xb5\x93\x4d\x91\xcc\x28\xa2\x0b\x3a\xc8"
"\xb9\xd4\x28\xbf\x3e\x5f\x0f\x6f\x4f\x03\xba\xe2\x79\xe5\xf3\x92\x1f\xdd\x02\x14\xde\x3f\x45\x20"
"\xba\x5a\x16\xd2\x3e\x78\x0f\x86\xba\xd5\xc0\xa5\x4f\x4f\xc2\x41\x1a\xd3\x69\xbf\xce\x68\x3c\x41"
"\xc2\x2b\xd5\xf5\xdb\x3e\xb1\x00\xe7\x0f\xf1\x46\x25\x40\x65\x16\x72\xdd\x8f\xc6\x08\x36\xd2\x60"
"\x39\x23\xfb\x80\x68\x28\x41\xfb\xe3\x8d\xea\x34\x07\xe4\x37\xe4\xf2\xfa\xf4\x63\x64\x15\x90\x4a"
"\x72\x68\x0c\xfc\xc5\xa3\x39\x6c\xa0\x52\x2d\x68\x33\x5a\x0c\xed\xd5\x7e\x7f\x04\xfb\x92\x54\xa1"
"\xbe\x01\x00\x00";

void TestQWeatherParse() {
    ink::WeatherData data;
    assert(ink::ParseQWeather(kNowJson, kDailyJson, kIndicesJson, "杭州", &data));
    assert(data.valid && data.city == "杭州");
    assert(data.temp == 15 && data.code == 305 && data.desc == "小雨");
    assert(data.low == 14 && data.high == 19);
    assert(data.humidity == 91);
    assert(data.wind_dir == "东风" && data.wind_level == "3级");
    assert(data.life == "穿衣 较冷 · 紫外线 最弱 · 运动 较不宜");
    assert(data.sunrise == "06:17" && data.sunset == "17:07");
    assert(data.advice == "有雨记得带伞，注意路滑");
    // 7 days come back; the page shows 4. Labels come from daily[0].fxDate.
    assert(data.forecast_count == 4);
    assert(data.forecast[0].day == "今天" && data.forecast[0].date == "11/07");
    assert(data.forecast[0].code == 305 && data.forecast[0].desc == "小雨");
    assert(data.forecast[0].temp_min == 14 && data.forecast[0].temp_max == 19);
    assert(data.forecast[1].day == "明天" && data.forecast[1].temp_min == 12);
    assert(data.forecast[2].day == "后天" && data.forecast[2].desc == "多云");
    assert(data.forecast[3].day == "周二" && data.forecast[3].date == "11/10");
    assert(data.forecast[3].desc == "晴" && data.forecast[3].temp_max == 16);

    data.fetched_at = 1794000000;
    ink::WeatherData restored;
    assert(ink::DeserializeWeather(ink::SerializeWeather(data), &restored));
    assert(restored.valid && restored.city == data.city && restored.temp == data.temp);
    assert(restored.desc == "小雨" && restored.humidity == data.humidity);
    assert(restored.wind_dir == data.wind_dir && restored.life == data.life);
    assert(restored.sunrise == data.sunrise && restored.sunset == data.sunset);
    assert(restored.advice == data.advice && restored.fetched_at == data.fetched_at);
    assert(restored.forecast_count == 4 && restored.forecast[3].day == "周二");
    assert(restored.forecast[2].desc == "多云");

    // Life indices are optional.
    ink::WeatherData no_life;
    assert(ink::ParseQWeather(kNowJson, kDailyJson, nullptr, "杭州", &no_life));
    assert(no_life.valid && no_life.life.empty() && no_life.forecast_count == 4);
    assert(ink::ParseQWeather(kNowJson, kDailyJson, R"({"code":"403"})", "杭州", &no_life));
    assert(no_life.life.empty());

    // Unknown categories are dropped so no glyph falls outside the font subset.
    const char kOddIndices[] = R"({"code":"200","daily":[{"type":"3","category":"冷热交替"},{"type":"5","category":"强"}]})";
    assert(ink::ParseQWeather(kNowJson, kDailyJson, kOddIndices, "杭州", &no_life));
    assert(no_life.life == "紫外线 强");

    // Range wind scales ("1-3") and missing wind360 in now.
    const char kSparseNow[] = R"({"code":"200","now":{"temp":"-3","icon":"999","windScale":"1-3"}})";
    assert(ink::ParseQWeather(kSparseNow, kDailyJson, nullptr, "杭州", &no_life));
    assert(no_life.temp == -3 && no_life.desc == "未知" && no_life.wind_level == "1级");
    assert(no_life.wind_dir.empty() && no_life.humidity == 93);

    // now and daily are required; error payloads fail.
    const char kError[] = R"({"error":{"status":400,"title":"No Such Location"}})";
    assert(!ink::ParseQWeather(kError, kDailyJson, kIndicesJson, "x", &data));
    assert(!ink::ParseQWeather(kNowJson, kError, kIndicesJson, "x", &data));
    assert(!ink::ParseQWeather(R"({"code":"204","now":{}})", kDailyJson, nullptr, "x", &data));
    assert(!ink::ParseQWeather("not json", kDailyJson, nullptr, "x", &data));
    assert(!ink::ParseQWeather(nullptr, kDailyJson, nullptr, "x", &data));

    // Caches from the Open-Meteo (no version) and Seniverse (v2) builds are rejected.
    assert(!ink::DeserializeWeather(R"({"city":"杭州","temp":12,"code":61,"fc":[]})", &restored));
    assert(!ink::DeserializeWeather(R"({"v":2,"city":"杭州","temp":12,"code":13,"fc":[]})", &restored));
    assert(!ink::DeserializeWeather(R"({"v":3,"city":"杭州","temp":12,"code":305,"fc":[]})", &restored));
    assert(!ink::DeserializeWeather("", &restored));
}

void TestLocation() {
    std::string id;
    assert(ink::ParseQWeatherLocation(kGeoJson, &id) && id == "101210101");
    assert(!ink::ParseQWeatherLocation(R"({"code":"404"})", &id));
    assert(!ink::ParseQWeatherLocation(R"({"error":{"status":400}})", &id));
    assert(!ink::ParseQWeatherLocation("", &id));

    const std::string cached = ink::SerializeCachedLocation("杭州", "101210101");
    std::string out;
    assert(ink::ParseCachedLocation(cached, "杭州", &out) && out == "101210101");
    assert(!ink::ParseCachedLocation(cached, "北京", &out));
    assert(!ink::ParseCachedLocation("", "杭州", &out));
}

void TestGzip() {
    const std::string raw(kNowGzip, sizeof(kNowGzip) - 1);
    assert(ink::IsGzip(raw) && !ink::IsGzip(kNowJson));
    std::string body;
    assert(ink::Gunzip(raw, 16 * 1024, &body) && body == kNowJson);
    assert(!ink::Gunzip(raw, 100, &body));                          // over the output cap
    assert(!ink::Gunzip(raw.substr(0, raw.size() - 10), 16 * 1024, &body));  // truncated
    assert(!ink::Gunzip("not gzip", 16 * 1024, &body));
}

void TestWeatherHelpers() {
    assert(std::strcmp(ink::WindDirFromDegrees(0), "北风") == 0);
    assert(std::strcmp(ink::WindDirFromDegrees(359), "北风") == 0);
    assert(std::strcmp(ink::WindDirFromDegrees(225), "西南风") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(100), "晴") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(153), "晴间多云") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(304), "雷阵雨伴有冰雹") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(456), "阵雨夹雪") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(502), "霾") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(515), "特强浓雾") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(901), "冷") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(999), "未知") == 0);
    assert(std::strcmp(ink::QWeatherCodeToDesc(-1), "未知") == 0);
    assert(ink::KnownLifeBrief("较舒适") && !ink::KnownLifeBrief("") && !ink::KnownLifeBrief(nullptr));
}

void TestSpeech() {
    assert(ink::NumberToChinese(0) == "零");
    assert(ink::NumberToChinese(7) == "七");
    assert(ink::NumberToChinese(10) == "十");
    assert(ink::NumberToChinese(15) == "十五");
    assert(ink::NumberToChinese(20) == "二十");
    assert(ink::NumberToChinese(38) == "三十八");
    assert(ink::NumberToChinese(-3) == "零下三");
    assert(ink::NumberToChinese(-12) == "零下十二");
    assert(ink::NumberToChinese(100) == "一百");
    assert(ink::NumberToChinese(105) == "一百零五");
    assert(ink::NumberToChinese(110) == "一百一十");

    ink::WeatherData data;
    assert(ink::ParseQWeather(kNowJson, kDailyJson, kIndicesJson, "杭州", &data));
    assert(ink::BuildWeatherSpeech(data) ==
           "杭州现在小雨，十五度。今天最低十四度，最高十九度，东风三级，湿度百分之九十一。"
           "穿衣较冷，紫外线最弱，运动较不宜。有雨记得带伞，注意路滑。明天小雨，十二到十七度。");

    ink::WeatherData sparse;
    assert(ink::ParseQWeather(R"({"code":"200","now":{"temp":"-3","icon":"100"}})", kDailyJson, nullptr,
                              "北京", &sparse));
    const std::string text = ink::BuildWeatherSpeech(sparse);
    assert(text.find("北京现在晴，零下三度。") == 0);
    assert(text.find("风") == std::string::npos && text.find("穿衣") == std::string::npos);
    assert(ink::BuildWeatherSpeech(ink::WeatherData()).empty());
}

void TestBroadcastSchedule() {
    // 06:59:30 -> 07:00 is 30 s away; 07:00:00 itself counts as tomorrow.
    assert(ink::SecondsUntilMinuteOfDay(6, 59, 30, 420) == 30);
    assert(ink::SecondsUntilMinuteOfDay(7, 0, 0, 420) == 86400);
    assert(ink::SecondsUntilMinuteOfDay(7, 0, 1, 420) == 86399);
    assert(ink::SecondsUntilMinuteOfDay(23, 0, 0, 420) == 8 * 3600);
    assert(ink::SecondsUntilMinuteOfDay(0, 0, 0, 0) == 86400);
}

#ifdef INK_TEST_JWT
std::string FromHex(const char* hex) {
    std::string out;
    for (; hex[0] && hex[1]; hex += 2) {
        out += static_cast<char>(std::stoi(std::string(hex, 2), nullptr, 16));
    }
    return out;
}

void TestJwt() {
    // RFC 8032 section 7.1, test 1 (empty message).
    const std::string seed = FromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    assert(ink::Ed25519SignBase64Url(seed, "") ==
           "5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc-bRr0lv18FlbviRlUUFDjnoQCw");
    assert(ink::Ed25519SignBase64Url("short", "x").empty());

    assert(ink::Base64UrlEncode("") == "");
    assert(ink::Base64UrlEncode("\xfb\xff") == "-_8");
    assert(ink::Base64UrlEncode("ab") == "YWI");

    // Same seed, standard Base64 as stored in sdkconfig; placeholder IDs only.
    const std::string seed_b64 = "nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A=";
    const std::string jwt = ink::BuildQWeatherJwt("YOUR_KEY_ID", "YOUR_PROJECT_ID", "YOUR_DEVELOPER_ID",
                                                  seed_b64, 1703912430);
    const size_t dot1 = jwt.find('.');
    const size_t dot2 = jwt.find('.', dot1 + 1);
    assert(dot1 != std::string::npos && dot2 != std::string::npos);
    assert(jwt.substr(0, dot1) == ink::Base64UrlEncode(R"({"alg":"EdDSA","kid":"YOUR_KEY_ID"})"));
    assert(jwt.substr(dot1 + 1, dot2 - dot1 - 1) ==
           ink::Base64UrlEncode(
               R"({"iss":"YOUR_DEVELOPER_ID","sub":"YOUR_PROJECT_ID","iat":1703912400,"exp":1703913330})"));
    assert(jwt.substr(dot2 + 1) == ink::Ed25519SignBase64Url(seed, jwt.substr(0, dot2)));
    assert(jwt.find_first_of("+/=") == std::string::npos);

    assert(ink::BuildQWeatherJwt("k", "s", "i", "", 1).empty());
    assert(ink::BuildQWeatherJwt("k", "s", "i", "bm90IDMyIGJ5dGVz", 1).empty());  // "not 32 bytes"
    assert(ink::BuildQWeatherJwt("", "s", "i", seed_b64, 1).empty());

    assert(ink::UrlEncode("杭州 a+b") == "%E6%9D%AD%E5%B7%9E%20a%2Bb");
}
#endif

void TestCalendarTables() {
    assert(std::strcmp(ink::LunarFestival(1, 1), "春节") == 0);
    assert(std::strcmp(ink::LunarFestival(8, 15), "中秋节") == 0);
    assert(ink::LunarFestival(3, 3) == nullptr);
    assert(std::strcmp(ink::SolarFestival(10, 1), "国庆节") == 0);
    assert(ink::SolarFestival(10, 2) == nullptr);

    bool in_table = false;
    assert(std::strcmp(ink::SolarTermExact(2026, 4, 5, &in_table), "清明") == 0 && in_table);
    assert(ink::SolarTermExact(2026, 4, 4, &in_table) == nullptr && in_table);
    assert(ink::SolarTermExact(2031, 4, 5, &in_table) == nullptr && !in_table);

    assert(std::strcmp(ink::LunarDayName(1), "初一") == 0);
    assert(std::strcmp(ink::LunarDayName(30), "三十") == 0);
    assert(std::strcmp(ink::WeekdayHeader(6), "日") == 0);
    assert(ink::DaysInMonth(2028, 2) == 29 && ink::DaysInMonth(2026, 2) == 28);
    assert(ink::WeekdayMondayFirst(2026, 11, 7) == 5);  // Saturday
    assert(ink::WeekdayMondayFirst(2026, 2, 16) == 0);  // Monday
    assert(ink::DailyTip(0) && ink::DailyTip(365));

    ink::LunarDate lunar = {};
    assert(ink::SolarToLunar(2026, 2, 17, &lunar));
    assert(lunar.year == 2026 && lunar.month == 1 && lunar.day == 1 && !lunar.is_leap_month);
    assert(ink::SolarToLunar(2026, 9, 25, &lunar) && lunar.month == 8 && lunar.day == 15);
    assert(ink::SolarToLunar(2026, 2, 16, &lunar) && lunar.year == 2025 && lunar.month == 12);
    assert(ink::SolarToLunar(2025, 7, 25, &lunar) && lunar.month == 6 && lunar.is_leap_month);
    assert(!ink::SolarToLunar(2101, 1, 1, &lunar));

    assert(ink::GanzhiDayIndex(1900, 1, 1) == 10);
    assert(std::strcmp(ink::StemName(10), "甲") == 0 && std::strcmp(ink::BranchName(10), "戌") == 0);
    assert(std::strcmp(ink::LunarMonthName(1), "正月") == 0);
    assert(std::strcmp(ink::LunarMonthName(12), "腊月") == 0);
    assert(std::strcmp(ink::ZodiacName(2026), "马") == 0);

    assert(ink::ShichenIndex(0) == 0 && ink::ShichenIndex(23) == 0);
    assert(ink::ShichenIndex(17) == 9 && ink::ShichenIndex(19) == 10);
    assert(std::strcmp(ink::ShichenName(9), "酉") == 0);
    int sh = -1, eh = -1;
    ink::ShichenHours(9, &sh, &eh);
    assert(sh == 17 && eh == 19);
    ink::ShichenHours(0, &sh, &eh);
    assert(sh == 23 && eh == 1);
    assert(ink::HourPillarIndex(10, 0) == 0);  // 甲戌日 子时 → 甲子
    assert(ink::ShichenLucky(0, 0));           // 子日 子时 青龙
    assert(!ink::ShichenLucky(0, 2));          // 子日 寅时 天刑

    int days = -1;
    assert(std::strcmp(ink::NextSolarTerm(2026, 9, 17, &days), "秋分") == 0 && days == 6);
    assert(std::strcmp(ink::NextSolarTerm(2026, 9, 23, &days), "秋分") == 0 && days == 0);
    assert(ink::NextSolarTerm(2031, 1, 1, &days) == nullptr);
    assert(ink::DaysUntilGaokao(2026, 9, 17) == 263);
    assert(ink::DaysUntilGaokao(2026, 6, 7) == 0);
}

void DumpLunar() {
    for (int y = 1999; y <= 2100; ++y) {
        for (int m = 1; m <= 12; ++m) {
            for (int d = 1; d <= ink::DaysInMonth(y, m); ++d) {
                ink::LunarDate l = {};
                if (ink::SolarToLunar(y, m, d, &l)) {
                    std::cout << y << '-' << m << '-' << d << ' ' << l.year << ' ' << l.month << ' '
                              << l.day << ' ' << (l.is_leap_month ? 1 : 0) << '\n';
                }
            }
        }
    }
}

void DumpTerms() {
    for (int y = 2024; y <= 2030; ++y) {
        for (int m = 1; m <= 12; ++m) {
            for (int d = 1; d <= ink::DaysInMonth(y, m); ++d) {
                bool in_table = false;
                if (const char* name = ink::SolarTermExact(y, m, d, &in_table)) {
                    std::cout << y << '-' << m << '-' << d << ' ' << name << '\n';
                }
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--dump-terms") {
        DumpTerms();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--dump-lunar") {
        DumpLunar();
        return 0;
    }
    TestQWeatherParse();
    TestLocation();
    TestGzip();
    TestWeatherHelpers();
    TestSpeech();
    TestBroadcastSchedule();
#ifdef INK_TEST_JWT
    TestJwt();
#endif
    TestCalendarTables();
    std::cout << "ink_data_test: all checks passed\n";
    return 0;
}
