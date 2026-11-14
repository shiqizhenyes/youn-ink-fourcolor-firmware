/**
 * @file ink_calendar_data.cc
 * @brief Festival, solar term and tip tables for the InkSight-style calendar
 */

#include "ink_calendar_data.h"

#include <cstdint>

namespace ink {

namespace {

constexpr int kLunarFirstYear = 1999;
constexpr int kLunarLastYear = 2100;

// zhdate CHINESEYEARCODE: bits 4-15 = month 1..12 sizes (1 = 30 days),
// bits 0-3 = leap month (0 = none), bit 16 = leap month has 30 days.
const uint32_t kLunarYearCode[] = {
    0x092E0, 0x0C960, 0x0D954, 0x0D4A0, 0x0DA50, 0x07552, 0x056A0, 0x0ABB7,  // 1999
    0x025D0, 0x092D0, 0x0CAB5, 0x0A950, 0x0B4A0, 0x0BAA4, 0x0AD50, 0x055D9,  // 2007
    0x04BA0, 0x0A5B0, 0x15176, 0x052B0, 0x0A930, 0x07954, 0x06AA0, 0x0AD50,  // 2015
    0x05B52, 0x04B60, 0x0A6E6, 0x0A4E0, 0x0D260, 0x0EA65, 0x0D530, 0x05AA0,  // 2023
    0x076A3, 0x096D0, 0x04AFB, 0x04AD0, 0x0A4D0, 0x1D0B6, 0x0D250, 0x0D520,  // 2031
    0x0DD45, 0x0B5A0, 0x056D0, 0x055B2, 0x049B0, 0x0A577, 0x0A4B0, 0x0AA50,  // 2039
    0x1B255, 0x06D20, 0x0ADA0, 0x14B63, 0x09370, 0x049F8, 0x04970, 0x064B0,  // 2047
    0x168A6, 0x0EA50, 0x06AA0, 0x1A6C4, 0x0AAE0, 0x092E0, 0x0D2E3, 0x0C960,  // 2055
    0x0D557, 0x0D4A0, 0x0DA50, 0x05D55, 0x056A0, 0x0A6D0, 0x055D4, 0x052D0,  // 2063
    0x0A9B8, 0x0A950, 0x0B4A0, 0x0B6A6, 0x0AD50, 0x055A0, 0x0ABA4, 0x0A5B0,  // 2071
    0x052B0, 0x0B273, 0x06930, 0x07337, 0x06AA0, 0x0AD50, 0x14B55, 0x04B60,  // 2079
    0x0A570, 0x054E4, 0x0D160, 0x0E968, 0x0D520, 0x0DAA0, 0x16AA6, 0x056D0,  // 2087
    0x04AE0, 0x0A9D4, 0x0A2D0, 0x0D150, 0x0F252, 0x0D520,  // 2095
};

// Lunar new year as (month << 5) | day.
const uint16_t kLunarNewYear[] = {
    0x050, 0x045, 0x038, 0x04C, 0x041, 0x036, 0x049, 0x03D, 0x052, 0x047,  // 1999
    0x03A, 0x04E, 0x043, 0x037, 0x04A, 0x03F, 0x053, 0x048, 0x03C, 0x050,  // 2009
    0x045, 0x039, 0x04C, 0x041, 0x036, 0x04A, 0x03D, 0x051, 0x046, 0x03A,  // 2019
    0x04D, 0x043, 0x037, 0x04B, 0x03F, 0x053, 0x048, 0x03C, 0x04F, 0x044,  // 2029
    0x038, 0x04C, 0x041, 0x036, 0x04A, 0x03E, 0x051, 0x046, 0x03A, 0x04E,  // 2039
    0x042, 0x037, 0x04B, 0x041, 0x053, 0x048, 0x03C, 0x04F, 0x044, 0x038,  // 2049
    0x04C, 0x042, 0x035, 0x049, 0x03D, 0x051, 0x045, 0x03A, 0x04E, 0x043,  // 2059
    0x037, 0x04B, 0x03F, 0x053, 0x047, 0x03B, 0x04F, 0x045, 0x038, 0x04C,  // 2069
    0x042, 0x036, 0x049, 0x03D, 0x051, 0x046, 0x03A, 0x04E, 0x043, 0x038,  // 2079
    0x04A, 0x03E, 0x052, 0x047, 0x03B, 0x04F, 0x045, 0x039, 0x04C, 0x041,  // 2089
    0x035, 0x049,  // 2099
};

int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t LunarNewYearDays(int lunar_year) {
    const uint16_t packed = kLunarNewYear[lunar_year - kLunarFirstYear];
    return DaysFromCivil(lunar_year, packed >> 5, packed & 0x1F);
}

struct DateName {
    int month;
    int day;
    const char* name;
};

const DateName kSolarFestivals[] = {
    {1, 1, "元旦"},   {2, 14, "情人节"}, {3, 8, "妇女节"},  {4, 1, "愚人节"},
    {5, 1, "劳动节"}, {6, 1, "儿童节"},  {10, 1, "国庆节"}, {12, 25, "圣诞节"},
};

const DateName kLunarFestivals[] = {
    {1, 1, "春节"},   {1, 15, "元宵节"}, {5, 5, "端午节"}, {7, 7, "七夕节"},
    {8, 15, "中秋节"}, {9, 9, "重阳节"}, {12, 8, "腊八节"},
};

const char* const kTermNames[24] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨",
    "立夏", "小满", "芒种", "夏至", "小暑", "大暑", "立秋", "处暑",
    "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};

constexpr int kTermFirstYear = 2024;
constexpr int kTermLastYear = 2030;

// Day of month for each of the 24 terms; two terms per month, January first.
const unsigned char kTermDays[kTermLastYear - kTermFirstYear + 1][24] = {
    {6, 20, 4, 19, 5, 20, 4, 19, 5, 20, 5, 21, 6, 22, 7, 22, 7, 22, 8, 23, 7, 22, 6, 21},  // 2024
    {5, 20, 3, 18, 5, 20, 4, 20, 5, 21, 5, 21, 7, 22, 7, 23, 7, 22, 8, 23, 7, 22, 7, 21},  // 2025
    {5, 20, 4, 18, 5, 20, 5, 20, 5, 21, 5, 21, 7, 23, 7, 23, 7, 23, 8, 23, 7, 22, 7, 22},  // 2026
    {5, 20, 4, 19, 6, 21, 5, 20, 6, 21, 6, 21, 7, 23, 7, 23, 8, 23, 8, 23, 7, 22, 7, 22},  // 2027
    {6, 21, 4, 19, 5, 20, 4, 19, 5, 20, 5, 21, 6, 22, 7, 22, 7, 22, 8, 23, 7, 22, 6, 21},  // 2028
    {5, 20, 3, 18, 5, 20, 4, 20, 5, 21, 5, 21, 7, 22, 7, 23, 7, 22, 8, 23, 7, 22, 7, 21},  // 2029
    {5, 20, 4, 18, 5, 20, 5, 20, 5, 21, 5, 21, 7, 22, 7, 23, 7, 23, 8, 23, 7, 22, 7, 22},  // 2030
};

const char* const kLunarDayNames[30] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
};

const char* const kLunarMonthNames[12] = {
    "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "十一月", "腊月",
};

const char* const kZodiacNames[12] = {
    "鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪",
};

const char* const kStemNames[10] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
const char* const kBranchNames[12] = {
    "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥",
};

const char* const kWeekdayHeaders[7] = {"一", "二", "三", "四", "五", "六", "日"};

int WrapMod(int v, int m) {
    int r = v % m;
    return r < 0 ? r + m : r;
}

const char* const kDailyTips[] = {
    "日拱一卒 功不唐捐",
    "千里之行 始于足下",
    "不积跬步 无以至千里",
    "少壮不努力 老大徒伤悲",
    "一年之计在于春",
    "一日之计在于晨",
    "业精于勤 荒于嬉",
    "读书破万卷 下笔如有神",
    "学而不思则罔",
    "知足者常乐",
    "行百里者半九十",
    "水滴石穿 绳锯木断",
    "宁静致远 淡泊明志",
    "天行健 君子以自强不息",
    "慢慢来 比较快",
    "今日事 今日毕",
};

}  // namespace

const char* SolarFestival(int month, int day) {
    for (const auto& f : kSolarFestivals) {
        if (f.month == month && f.day == day) return f.name;
    }
    return nullptr;
}

const char* LunarFestival(int lunar_month, int lunar_day) {
    for (const auto& f : kLunarFestivals) {
        if (f.month == lunar_month && f.day == lunar_day) return f.name;
    }
    return nullptr;
}

const char* SolarTermExact(int year, int month, int day, bool* in_table) {
    const bool covered = year >= kTermFirstYear && year <= kTermLastYear;
    if (in_table) *in_table = covered;
    if (!covered || month < 1 || month > 12) return nullptr;
    const unsigned char* days = kTermDays[year - kTermFirstYear];
    for (int k = 0; k < 2; ++k) {
        const int idx = (month - 1) * 2 + k;
        if (days[idx] == day) return kTermNames[idx];
    }
    return nullptr;
}

const char* LunarDayName(int lunar_day) {
    if (lunar_day < 1 || lunar_day > 30) return "";
    return kLunarDayNames[lunar_day - 1];
}

const char* LunarMonthName(int lunar_month) {
    if (lunar_month < 1 || lunar_month > 12) return "";
    return kLunarMonthNames[lunar_month - 1];
}

const char* ZodiacName(int lunar_year) {
    return kZodiacNames[WrapMod(lunar_year - 4, 12)];
}

const char* StemName(int index) {
    return kStemNames[WrapMod(index, 10)];
}

const char* BranchName(int index) {
    return kBranchNames[WrapMod(index, 12)];
}

int GanzhiDayIndex(int year, int month, int day) {
    const int64_t delta = DaysFromCivil(year, month, day) - DaysFromCivil(1900, 1, 1);
    return WrapMod(static_cast<int>(delta % 60) + 10, 60);
}

int ShichenIndex(int hour) {
    return WrapMod((hour + 1) / 2, 12);
}

const char* ShichenName(int shichen) {
    return BranchName(shichen);
}

void ShichenHours(int shichen, int* start_hour, int* end_hour) {
    const int idx = WrapMod(shichen, 12);
    if (start_hour) *start_hour = WrapMod(idx * 2 + 23, 24);
    if (end_hour) *end_hour = WrapMod(idx * 2 + 1, 24);
}

int HourPillarIndex(int day_gz, int shichen) {
    const int zi_stem = WrapMod(day_gz, 10) % 5 * 2;
    return WrapMod(zi_stem * 6 + WrapMod(shichen, 12), 60);
}

bool ShichenLucky(int day_zhi, int shichen) {
    const int god = WrapMod(WrapMod(shichen, 12) + WrapMod(day_zhi, 12), 12);
    static const bool kLucky[] = {true, true, false, false, true, true,
                                  false, true, false, false, true, false};
    return kLucky[god];
}

const char* NextSolarTerm(int year, int month, int day, int* days_until) {
    if (days_until) *days_until = -1;
    if (year > kTermLastYear) return nullptr;
    const int start_year = year < kTermFirstYear ? kTermFirstYear : year;
    const int64_t today = DaysFromCivil(year, month, day);
    for (int y = start_year; y <= kTermLastYear; ++y) {
        const unsigned char* days = kTermDays[y - kTermFirstYear];
        for (int i = 0; i < 24; ++i) {
            const int tm = i / 2 + 1;
            const int64_t t = DaysFromCivil(y, tm, days[i]);
            if (t < today) continue;
            if (days_until) *days_until = static_cast<int>(t - today);
            return kTermNames[i];
        }
    }
    return nullptr;
}

int DaysUntilGaokao(int year, int month, int day) {
    int gy = year;
    if (month > 6 || (month == 6 && day > 7)) ++gy;
    return static_cast<int>(DaysFromCivil(gy, 6, 7) - DaysFromCivil(year, month, day));
}

const char* WeekdayHeader(int index) {
    if (index < 0 || index > 6) return "";
    return kWeekdayHeaders[index];
}

const char* DailyTip(int day_of_year) {
    constexpr int kCount = sizeof(kDailyTips) / sizeof(kDailyTips[0]);
    int idx = day_of_year % kCount;
    if (idx < 0) idx += kCount;
    return kDailyTips[idx];
}

int DaysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

int WeekdayMondayFirst(int year, int month, int day) {
    static const int kOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) year -= 1;
    const int sunday_first = (year + year / 4 - year / 100 + year / 400 + kOffsets[month - 1] + day) % 7;
    return (sunday_first + 6) % 7;
}

bool SolarToLunar(int year, int month, int day, LunarDate* out) {
    if (!out || year < kLunarFirstYear || year > kLunarLastYear) return false;
    const int64_t target = DaysFromCivil(year, month, day);
    int lunar_year = year;
    if (target < LunarNewYearDays(lunar_year)) --lunar_year;
    if (lunar_year < kLunarFirstYear) return false;

    const uint32_t code = kLunarYearCode[lunar_year - kLunarFirstYear];
    const int leap = static_cast<int>(code & 0xF);
    int64_t remaining = target - LunarNewYearDays(lunar_year);

    // Walk months in calendar order; the leap month follows its regular month.
    for (int m = 1; m <= 12; ++m) {
        const int size = ((code >> (16 - m)) & 1) ? 30 : 29;
        if (remaining < size) {
            *out = {lunar_year, m, static_cast<int>(remaining) + 1, false};
            return true;
        }
        remaining -= size;
        if (m == leap) {
            const int leap_size = (code >> 16) ? 30 : 29;
            if (remaining < leap_size) {
                *out = {lunar_year, m, static_cast<int>(remaining) + 1, true};
                return true;
            }
            remaining -= leap_size;
        }
    }
    return false;
}

}  // namespace ink
