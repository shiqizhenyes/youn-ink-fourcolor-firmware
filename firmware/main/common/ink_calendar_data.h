/**
 * @file ink_calendar_data.h
 * @brief Festival, solar term and tip tables for the InkSight-style calendar
 *
 * Tables are ported from InkSight backend/core/config.py. Pure data, no
 * ESP-IDF dependency.
 */

#ifndef INK_CALENDAR_DATA_H
#define INK_CALENDAR_DATA_H

namespace ink {

const char* SolarFestival(int month, int day);
const char* LunarFestival(int lunar_month, int lunar_day);

/**
 * @brief Exact solar term for a date in 2024-2030
 * @param in_table Set to true when the year is covered by the table
 * @return Term name, or nullptr when the date is not a solar term
 */
const char* SolarTermExact(int year, int month, int day, bool* in_table);

const char* LunarDayName(int lunar_day);    // 初一..三十
const char* LunarMonthName(int lunar_month);  // 正月..腊月
const char* ZodiacName(int lunar_year);     // 鼠..猪
const char* WeekdayHeader(int index);       // 0=一 .. 6=日

const char* StemName(int index);    // 甲..癸, index % 10
const char* BranchName(int index);  // 子..亥, index % 12

/** @brief 日干支 index 0=甲子; 1900-01-01 is 甲戌 (10) */
int GanzhiDayIndex(int year, int month, int day);

/** @brief 0=子 (23–01) .. 11=亥 (21–23) */
int ShichenIndex(int hour);
const char* ShichenName(int shichen);
void ShichenHours(int shichen, int* start_hour, int* end_hour);

/** @brief 时干支 index (五鼠遁) */
int HourPillarIndex(int day_gz, int shichen);

/** @brief 黄道吉时: 青龙/明堂/金匮/天德/玉堂/司命 */
bool ShichenLucky(int day_zhi, int shichen);

/**
 * @brief Next solar term on or after the date (2024–2030)
 * @param days_until 0 if the date itself is a solar term
 * @return Term name, or nullptr when the date is outside the table
 */
const char* NextSolarTerm(int year, int month, int day, int* days_until);

/** @brief Days until the next 7 June gaokao; 0 on the day itself */
int DaysUntilGaokao(int year, int month, int day);

/**
 * @brief Offline replacement for InkSight's LLM tip line, rotated by day of year
 */
const char* DailyTip(int day_of_year);

int DaysInMonth(int year, int month);
int WeekdayMondayFirst(int year, int month, int day);  // 0=Mon .. 6=Sun

struct LunarDate {
    int year;
    int month;  // 1..12
    int day;    // 1..30
    bool is_leap_month;
};

/**
 * @brief Exact Gregorian -> lunar conversion for 1999-2100 (zhdate tables)
 * @return false when the date is outside the table range
 */
bool SolarToLunar(int year, int month, int day, LunarDate* out);

}  // namespace ink

#endif  // INK_CALENDAR_DATA_H
