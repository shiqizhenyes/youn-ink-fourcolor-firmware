/**
 * @file ink_calendar_renderer.cc
 * @brief Month calendar ported from InkSight's CALENDAR mode (calendar_grid block)
 */

#include "ink_calendar_renderer.h"

#include "almanac_ornaments.h"
#include "common/ink_calendar_data.h"
#include "ink_text_utils.h"
#include "inksight_assets.h"
#include "rawdraw/components/calendar.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

constexpr int kBodyTopGap = 8;
constexpr int kMarginX = 10;
constexpr int kHeaderPadY = 3;
constexpr int kHeaderGap = 5;
constexpr int kSolarLunarGap = 8;
constexpr int kWeekendStart = 5;
constexpr int kTodayPadX = 10;
constexpr int kTodayPadY = 3;
constexpr int kTipBandExtra = 16;
constexpr int kSeparatorMarginX = 5;
constexpr int kRows = 6;
const char* const kWeekdayBar[7] = {
    "星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日",
};
constexpr int kDailyMarginX = 12;
constexpr char kLuckyYi[] = "祈福 求嗣 订婚 嫁娶 出行 求财 开市 交易 安床 祭祀";

const lv_font_t* DayFont() { return &inksight_cjk_16; }
const lv_font_t* HeaderFont() { return &inksight_cjk_12; }
const lv_font_t* LabelFont() { return &inksight_cjk_12; }
const lv_font_t* TipFont() { return &inksight_cjk_14; }
const lv_font_t* BodyFont() { return &SourceHanSansSC_Regular_slim; }
const lv_font_t* TitleFont() { return &SourceHanSansSC_Medium_slim; }
const lv_font_t* BigDayFont() { return &inksight_temp_48; }

struct Today {
    int year;
    int month;
    int day;
    int day_of_year;
    int hour;
};

Today GetToday() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    return {tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_yday, tm_buf.tm_hour};
}

void DrawDashedHLine(uint8_t* fb, int width, int y, int x1, int x2, Color color) {
    constexpr int kDash = 4;
    constexpr int kGap = 3;
    for (int x = x1; x <= x2; x += kDash + kGap) {
        const int end = x + kDash - 1 < x2 ? x + kDash - 1 : x2;
        DrawHLine(fb, width, y, x, end, color);
    }
}

void FormatShichenTitle(int shichen, char* buf, size_t buf_size) {
    int start = 0;
    int end = 0;
    ink::ShichenHours(shichen, &start, &end);
    snprintf(buf, buf_size, "%s时(%d点至%d点)：", ink::ShichenName(shichen), start, end);
}

int MaxInk(int a, int b) { return a > b ? a : b; }

int ColumnX(int origin, int cell_w, int col) {
    return origin + col * cell_w;
}

int ColumnWidth(int origin, int cell_w, int grid_w, int col) {
    const int x = ColumnX(origin, cell_w, col);
    return col >= 6 ? origin + grid_w - x : cell_w;
}

int DrawJiDivider(uint8_t* fb, int width, int height, int y) {
    (void)height;
    const inksight_mono_bitmap_t& left = kAlmanacCloudLeft;
    const inksight_mono_bitmap_t& right = kAlmanacCloudRight;
    const inksight_mono_bitmap_t& ji = kAlmanacJi;
    constexpr int kGap = 4;
    const int h = MaxInk(ji.height, MaxInk(left.height, right.height));
    const int cy = y + h / 2;
    const int ji_x = (width - ji.width) / 2;
    const int ji_y = y + (h - ji.height) / 2;
    const int left_x = ji_x - kGap - left.width;
    const int left_y = y + (h - left.height) / 2;
    const int right_x = ji_x + ji.width + kGap;
    const int right_y = y + (h - right.height) / 2;

    DrawMonoBitmap(fb, width, left_x, left_y, left.width, left.height, left.bits, RED);
    DrawMonoBitmap(fb, width, ji_x, ji_y, ji.width, ji.height, ji.bits, RED);
    DrawMonoBitmap(fb, width, right_x, right_y, right.width, right.height, right.bits, RED);

    if (left_x - 2 >= kDailyMarginX) {
        DrawHLine(fb, width, cy, kDailyMarginX, left_x - 2, RED);
    }
    const int right_end = right_x + right.width + 1;
    if (right_end <= width - kDailyMarginX) {
        DrawHLine(fb, width, cy, right_end, width - kDailyMarginX, RED);
    }
    return y + h + 4;
}

void DrawYiJiCard(uint8_t* fb, int width, int height, const Rect& r, int shichen, bool lucky) {
    DrawRoundRect(fb, width, height, r, Style::kBorderRadiusMD, WHITE, BLACK, 1);

    char title[40];
    FormatShichenTitle(shichen, title, sizeof(title));
    const int pad = 8;
    int y = r.y + 6;
    ink_text::DrawAtInkTop(fb, width, height, r.x + pad, y, title, BodyFont(), BLACK);
    y += 18;
    DrawDashedHLine(fb, width, y, r.x + pad, r.x + r.w - pad - 1, BLACK);
    y += 8;

    const char* yi = lucky ? kLuckyYi : "无";
    char yi_line[96];
    snprintf(yi_line, sizeof(yi_line), "宜：%s", yi);
    const auto yi_lines = ink_text::Wrap(yi_line, BodyFont(), r.w - pad * 2, lucky ? 4 : 1);
    for (const auto& line : yi_lines) {
        ink_text::DrawAtInkTop(fb, width, height, r.x + pad, y, line.c_str(), BodyFont(), BLACK);
        y += 16;
    }

    const char* ji = lucky ? "忌：无" : "忌：诸事不宜";
    const int bar_h = 22;
    const int bar_y = r.y + r.h - pad - bar_h;
    DrawRect(fb, width, {r.x + pad, bar_y, r.w - pad * 2, bar_h}, RED);
    const int jw = MeasureTextWidth(ji, BodyFont());
    const ink_text::InkBox jb = ink_text::Measure(BodyFont(), ji);
    const int jx = r.x + pad + (r.w - pad * 2 - jw) / 2;
    const int jy = bar_y + (bar_h - jb.height) / 2;
    ink_text::DrawAtInkTop(fb, width, height, jx, jy, ji, BodyFont(), WHITE);
}

}  // namespace

InkCalendarRenderer::InkCalendarRenderer() {
    JumpToToday();
}

void InkCalendarRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    daily_view_ = false;
    JumpToToday();
}

void InkCalendarRenderer::JumpToToday() {
    const Today today = GetToday();
    year_ = today.year;
    month_ = today.month;
    day_ = today.day;
}

void InkCalendarRenderer::ShiftDay(int delta) {
    std::tm t = {};
    t.tm_year = year_ - 1900;
    t.tm_mon = month_ - 1;
    t.tm_mday = day_ + delta;
    t.tm_hour = 12;
    std::mktime(&t);
    year_ = t.tm_year + 1900;
    month_ = t.tm_mon + 1;
    day_ = t.tm_mday;
}

void InkCalendarRenderer::SetMonth(int year, int month) {
    year_ = year;
    month_ = month;
    needs_full_refresh_ = true;
}

std::string InkCalendarRenderer::GetTitle() const {
    if (daily_view_) return "老黄历";
    char buf[24];
    snprintf(buf, sizeof(buf), "%d年%d月", year_, month_);
    return buf;
}

InkCalendarRenderer::DayLabel InkCalendarRenderer::LabelForDate(int year, int month, int day) {
    ink::LunarDate lunar = {};
    const bool has_lunar = ink::SolarToLunar(year, month, day, &lunar);

    if (has_lunar && !lunar.is_leap_month) {
        if (const char* fest = ink::LunarFestival(lunar.month, lunar.day)) {
            return {fest, true};
        }
    }
    if (const char* fest = ink::SolarFestival(month, day)) {
        return {fest, true};
    }
    bool in_table = false;
    const char* term = ink::SolarTermExact(year, month, day, &in_table);
    if (!in_table) {
        term = Calendar::GetSolarTerm(month, day);
    }
    if (term && *term) {
        return {term, true};
    }
    if (has_lunar) {
        return {ink::LunarDayName(lunar.day), false};
    }
    return {};
}

void InkCalendarRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    DrawRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, WHITE);
    if (daily_view_) {
        RenderDaily(fb, width, height);
    } else {
        RenderMonth(fb, width, height);
    }
    needs_full_refresh_ = false;
}

void InkCalendarRenderer::RenderMonth(uint8_t* fb, int width, int height) {
    const Today today = GetToday();
    const bool showing_today_month = today.year == year_ && today.month == month_;

    const int grid_w = width - kMarginX * 2;
    const int cell_w = grid_w / 7;
    const int x0 = kMarginX;

    const ink_text::InkBox header_ink = ink_text::Measure(HeaderFont(), kWeekdayBar[0]);
    const int header_h = header_ink.height + kHeaderPadY * 2;
    const int header_y = Style::kStatusBarHeight + kBodyTopGap;
    const int weekday_w = cell_w * kWeekendStart;
    DrawRect(fb, width, {x0, header_y, weekday_w, header_h}, BLACK);
    DrawRect(fb, width, {x0 + weekday_w, header_y, grid_w - weekday_w, header_h}, RED);
    const int header_text_y = header_y + kHeaderPadY;
    for (int col = 0; col < 7; ++col) {
        const int col_x = ColumnX(x0, cell_w, col);
        const int col_w = ColumnWidth(x0, cell_w, grid_w, col);
        ink_text::DrawCenteredAtInkTop(fb, width, height, col_x + col_w / 2, header_text_y,
                                       kWeekdayBar[col], HeaderFont(), WHITE);
    }

    const int grid_top = header_y + header_h + kHeaderGap;
    const int first_col = ink::WeekdayMondayFirst(year_, month_, 1);
    const int days = ink::DaysInMonth(year_, month_);
    int rows = (first_col + days + 6) / 7;
    if (rows < 1) rows = 1;
    if (rows > kRows) rows = kRows;

    const ink_text::InkBox digit_box = ink_text::Measure(DayFont(), "0123456789");
    const ink_text::InkBox label_box = ink_text::Measure(LabelFont(), "初一");
    const int stack_h = digit_box.height + kSolarLunarGap + label_box.height;
    const int min_row_h = std::max(stack_h, 1);

    const char* tip = ink::DailyTip(today.day_of_year);
    const ink_text::InkBox tip_box = ink_text::Measure(TipFont(), tip);
    const int tip_h = tip_box.height > 0 ? tip_box.height : TipFont()->line_height;
    int band_h = tip_h + kTipBandExtra;
    const int room = height - grid_top;
    const int min_grid_h = min_row_h * rows;
    if (room < min_grid_h + 1 + band_h) {
        band_h = std::max(0, room - min_grid_h - 1);
    }
    const int grid_budget = std::max(0, room - 1 - band_h);
    int row_h = rows > 0 ? grid_budget / rows : min_row_h;
    if (row_h < min_row_h) row_h = min_row_h;

    for (int row = 0; row < rows; ++row) {
        const int row_y = grid_top + row * row_h;
        const int slack = std::max(0, row_h - stack_h);
        const int pad_y = std::min(kTodayPadY, slack / 2);
        const int solar_y = row_y + slack / 2;
        const int lunar_y = solar_y + digit_box.height + kSolarLunarGap;

        for (int col = 0; col < 7; ++col) {
            const int day = row * 7 + col - first_col + 1;
            if (day < 1 || day > days) continue;

            char num[12];
            snprintf(num, sizeof(num), "%d", day);
            const int col_x = ColumnX(x0, cell_w, col);
            const int col_w = ColumnWidth(x0, cell_w, grid_w, col);
            const int cx = col_x + col_w / 2;
            const int tw = MeasureTextWidth(num, DayFont());
            const bool is_today = showing_today_month && day == today.day;
            const DayLabel label = LabelForDate(year_, month_, day);
            const bool has_label = label.text && label.text[0];

            if (is_today) {
                const int label_w = has_label ? MeasureTextWidth(label.text, LabelFont()) : 0;
                const int text_w = std::max(tw, label_w);
                const int max_w = std::max(1, col_w - 2);
                int rect_w = std::min(max_w, text_w + kTodayPadX * 2);
                if (rect_w < 1) rect_w = max_w;
                const int rect_x = col_x + (col_w - rect_w) / 2;
                const int label_h = has_label ? ink_text::Measure(LabelFont(), label.text).height : 0;
                const int ink_top = solar_y;
                const int ink_bottom = has_label ? lunar_y + label_h : solar_y + digit_box.height;
                int rect_top = ink_top - pad_y;
                int rect_bottom = ink_bottom + pad_y;
                const int row_bottom = row_y + row_h;
                if (rect_top < row_y) rect_top = row_y;
                if (rect_bottom > row_bottom) rect_bottom = row_bottom;
                DrawRoundRect(fb, width, height, {rect_x, rect_top, rect_w, rect_bottom - rect_top},
                              Style::kBorderRadiusSM, RED, RED, 0);
            }

            const Color num_color = is_today ? WHITE : (col >= kWeekendStart ? RED : BLACK);
            ink_text::DrawAtInkTop(fb, width, height, cx - tw / 2, solar_y, num, DayFont(), num_color);
            if (has_label) {
                const Color label_color = is_today ? WHITE : (label.highlight ? RED : BLACK);
                ink_text::DrawCenteredAtInkTop(fb, width, height, cx, lunar_y, label.text, LabelFont(),
                                               label_color);
            }
        }
    }

    const int line_y = grid_top + row_h * rows;
    if (line_y >= 0 && line_y < height) {
        DrawDashedHLine(fb, width, line_y, kSeparatorMarginX, width - kSeparatorMarginX, BLACK);
        const int band_top = line_y + 1;
        const int band_actual = height - band_top;
        if (band_actual > 0 && tip && tip[0]) {
            const int text_top = band_actual > tip_h ? band_top + (band_actual - tip_h) / 2 : band_top;
            ink_text::DrawCenteredAtInkTop(fb, width, height, width / 2, text_top, tip, TipFont(), BLACK);
        }
    }
}

void InkCalendarRenderer::RenderDaily(uint8_t* fb, int width, int height) const {
    const Today now = GetToday();
    const int shichen = ink::ShichenIndex(now.hour);
    const int next_sc = (shichen + 1) % 12;
    const int day_gz = ink::GanzhiDayIndex(year_, month_, day_);
    const int hour_gz = ink::HourPillarIndex(day_gz, shichen);
    const int day_zhi = day_gz % 12;

    char ganzhi[24];
    snprintf(ganzhi, sizeof(ganzhi), "%s%s-%s%s", ink::StemName(day_gz), ink::BranchName(day_gz),
             ink::StemName(hour_gz), ink::BranchName(hour_gz));
    char date[16];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", year_, month_, day_);

    int y = Style::kStatusBarHeight + 6;
    ink_text::DrawAtInkTop(fb, width, height, kDailyMarginX, y, ganzhi, TitleFont(), RED);
    const int date_w = MeasureTextWidth(date, TitleFont());
    ink_text::DrawAtInkTop(fb, width, height, width - kDailyMarginX - date_w, y, date, TitleFont(), RED);
    y += ink_text::Measure(TitleFont(), ganzhi).height + 12;

    char day_num[8];
    snprintf(day_num, sizeof(day_num), "%d", day_);
    const int big_x = kDailyMarginX;
    const int big_w = MeasureTextWidth(day_num, BigDayFont());
    const int big_h = ink_text::Measure(BigDayFont(), day_num).height;

    ink::LunarDate lunar = {};
    char animal[24] = {};
    char lunar_line[32] = {};
    if (ink::SolarToLunar(year_, month_, day_, &lunar)) {
        snprintf(animal, sizeof(animal), "%s年", ink::ZodiacName(lunar.year));
        snprintf(lunar_line, sizeof(lunar_line), "%s%s%s", lunar.is_leap_month ? "闰" : "",
                 ink::LunarMonthName(lunar.month), ink::LunarDayName(lunar.day));
    }

    const int wd = ink::WeekdayMondayFirst(year_, month_, day_);
    int term_days = -1;
    const char* term = ink::NextSolarTerm(year_, month_, day_, &term_days);
    char wait_buf[24];
    const char* wait = nullptr;
    if (term) {
        if (term_days == 0) wait = "今天";
        else if (term_days > 0) {
            snprintf(wait_buf, sizeof(wait_buf), "还有%d天", term_days);
            wait = wait_buf;
        }
    }

    const int row_top = y;
    const int line1_h = MaxInk(MaxInk(ink_text::Measure(TitleFont(), ink::WeekdayHeader(wd)).height,
                                     term ? ink_text::Measure(TitleFont(), term).height : 0),
                               animal[0] ? ink_text::Measure(BodyFont(), animal).height : 0);
    const int line2_y = row_top + line1_h + 8;
    const int line2_h = MaxInk(MaxInk(ink_text::Measure(BodyFont(), "星期").height,
                                     wait ? ink_text::Measure(BodyFont(), wait).height : 0),
                               lunar_line[0] ? ink_text::Measure(BodyFont(), lunar_line).height : 0);
    const int lines_h = line2_y + line2_h - row_top;
    const int block_h = MaxInk(big_h, lines_h);
    const int big_y = big_h >= lines_h ? row_top : row_top + (lines_h - big_h) / 2;

    ink_text::DrawAtInkTop(fb, width, height, big_x, big_y, day_num, BigDayFont(), BLACK);
    const int rule_x = big_x + big_w + 6;
    DrawVLine(fb, width, rule_x, row_top + 2, row_top + lines_h - 2, BLACK);

    const int lunar_x = rule_x + 8;
    if (animal[0]) {
        ink_text::DrawAtInkTop(fb, width, height, lunar_x, row_top, animal, BodyFont(), BLACK);
    }
    if (lunar_line[0]) {
        ink_text::DrawAtInkTop(fb, width, height, lunar_x, line2_y, lunar_line, BodyFont(), BLACK);
    }

    const int right_x = width / 2 + 18;
    ink_text::DrawCenteredAtInkTop(fb, width, height, right_x + 12, row_top, ink::WeekdayHeader(wd), TitleFont(),
                                   BLACK);
    ink_text::DrawCenteredAtInkTop(fb, width, height, right_x + 12, line2_y, "星期", BodyFont(), BLACK);
    DrawVLine(fb, width, right_x + 36, row_top + 2, row_top + lines_h - 2, BLACK);

    const int term_x = right_x + 50;
    if (term) {
        ink_text::DrawAtInkTop(fb, width, height, term_x, row_top, term, TitleFont(), BLACK);
    }
    if (wait) {
        ink_text::DrawAtInkTop(fb, width, height, term_x, line2_y, wait, BodyFont(), BLACK);
    }

    y = DrawJiDivider(fb, width, height, row_top + block_h + 4);

    const int gap = 8;
    const int card_w = (width - kDailyMarginX * 2 - gap) / 2;
    const int card_h = height - 8 - y;
    if (card_h >= 70) {
        DrawYiJiCard(fb, width, height, {kDailyMarginX, y, card_w, card_h}, shichen,
                     ink::ShichenLucky(day_zhi, shichen));
        DrawYiJiCard(fb, width, height, {kDailyMarginX + card_w + gap, y, card_w, card_h}, next_sc,
                     ink::ShichenLucky(day_zhi, next_sc));
    }
}

bool InkCalendarRenderer::HandleInput(const ButtonEvent& event) {
    if (daily_view_) {
        switch (event.type) {
            case ButtonEvent::kUpClick:
                ShiftDay(-1);
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kDownClick:
                ShiftDay(1);
                needs_full_refresh_ = true;
                return true;
            case ButtonEvent::kBootClick:
                daily_view_ = false;
                needs_full_refresh_ = true;
                return true;
            default:
                return false;
        }
    }

    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (--month_ < 1) {
                month_ = 12;
                --year_;
            }
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownClick:
            if (++month_ > 12) {
                month_ = 1;
                ++year_;
            }
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kBootClick:
            JumpToToday();
            daily_view_ = true;
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

}  // namespace rawdraw
