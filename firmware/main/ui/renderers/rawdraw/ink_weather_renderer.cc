/**
 * @file ink_weather_renderer.cc
 * @brief Weather page ported from InkSight's WEATHER mode (weather.json layout)
 */

#include "ink_weather_renderer.h"

#include "ink_text_utils.h"
#include "inksight_assets.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"

#include <cstdio>
#include <string>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;

namespace rawdraw {

namespace {

constexpr int kCityMarginX = 20;
constexpr int kCitySpacer = 8;
constexpr int kLeftX = 15;
constexpr int kLeftWidth = 170;
constexpr int kColumnGap = 6;
constexpr int kCardsMarginX = -4;
constexpr int kCardsGap = 4;
constexpr int kInfoMarginX = 15;
constexpr int kSeparatorMarginX = 20;
constexpr int kAdviceMaxLines = 2;
constexpr int kAdviceMarginX = 24;  // InkSight default text margin: 6% of 400
constexpr int kInfoLineSlot = 12;
constexpr int kInfoLineGap = 6;
constexpr int kBeforeDividerGap = 8;
// Divider is drawn at y, then y += 8 + 1 + 6 before advice ink. Keep that 15px.
constexpr int kAdviceGap = 15;

const lv_font_t* CityFont() { return &SourceHanSansSC_Regular_slim; }
const lv_font_t* Serif12() { return &inksight_cjk_12; }
const lv_font_t* Serif14() { return &inksight_cjk_14; }
const lv_font_t* Serif16() { return &inksight_cjk_16; }
const lv_font_t* TempFont() { return &inksight_temp_48; }

inksight_icon_t IconForCode(int code) {
    if (code == 100 || code == 150 || code == 900) return INKSIGHT_ICON_SUNNY;
    if ((code >= 101 && code <= 103) || (code >= 151 && code <= 153)) {
        return INKSIGHT_ICON_PARTLY_CLOUDY;
    }
    if (code >= 302 && code <= 304) return INKSIGHT_ICON_THUNDERSTORM;
    if (code == 404 || code == 405 || code == 406 || code == 456) return INKSIGHT_ICON_SNOW;
    if (code >= 300 && code <= 399) return INKSIGHT_ICON_RAINY;
    if (code >= 400 && code <= 499) return INKSIGHT_ICON_SNOW;
    if (code >= 500 && code <= 515) return INKSIGHT_ICON_FOGGY;
    return INKSIGHT_ICON_CLOUD;
}

Color ColorForCode(int code) {
    switch (IconForCode(code)) {
        case INKSIGHT_ICON_RAINY:
        case INKSIGHT_ICON_THUNDERSTORM:
        case INKSIGHT_ICON_SNOW:
            return RED;
        case INKSIGHT_ICON_FOGGY:
            return YELLOW;
        default:
            return BLACK;
    }
}

void DrawWeatherIcon(uint8_t* fb, int width, int x, int y, int code, int size) {
    if (!fb || code < 0) return;
    const inksight_mono_bitmap_t* set = size >= 32 ? inksight_weather_icons_32 : inksight_weather_icons_24;
    const inksight_mono_bitmap_t& icon = set[IconForCode(code)];
    DrawMonoBitmap(fb, width, x, y, icon.width, icon.height, icon.bits, ColorForCode(code));
}

int PlaceAtInkTop(uint8_t* fb, int width, int height, int x, int ink_top, const char* text,
                  const lv_font_t* font, Color color) {
    if (!text || !*text) return 0;
    if (fb) return ink_text::DrawAtInkTop(fb, width, height, x, ink_top, text, font, color);
    return ink_text::Measure(font, text).height;
}

int PlaceCenteredAtInkTop(uint8_t* fb, int width, int height, int center_x, int ink_top,
                          const char* text, const lv_font_t* font, Color color) {
    if (!text || !*text) return 0;
    if (fb) {
        return ink_text::DrawCenteredAtInkTop(fb, width, height, center_x, ink_top, text, font, color);
    }
    return ink_text::Measure(font, text).height;
}

std::string HumidityWindLine(const ink::WeatherData& data) {
    std::string line = "湿度 ";
    line += data.humidity >= 0 ? std::to_string(data.humidity) : "--";
    line += "%";
    if (!data.wind_dir.empty() || !data.wind_level.empty()) {
        line += "   " + data.wind_dir + " " + data.wind_level;
    }
    return line;
}

std::string SunLine(const ink::WeatherData& data) {
    if (data.sunrise.empty() || data.sunset.empty()) return {};
    return "日出 " + data.sunrise + " | 日落 " + data.sunset;
}

}  // namespace

InkWeatherRenderer::InkWeatherRenderer() = default;

void InkWeatherRenderer::Init(int width, int height) {
    // SwitchPage() calls Init() on every page entry; keep the fetched data.
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
}

void InkWeatherRenderer::SetData(const ink::WeatherData& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_ = data;
    loading_ = false;
    failed_ = false;
    needs_full_refresh_ = true;
}

bool InkWeatherRenderer::HasData() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.valid;
}

void InkWeatherRenderer::SetLoading(bool loading) {
    std::lock_guard<std::mutex> lock(mutex_);
    loading_ = loading;
    if (loading) failed_ = false;
}

void InkWeatherRenderer::SetFailed(bool failed) {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = failed;
    if (failed) loading_ = false;
}

void InkWeatherRenderer::SetUnconfigured(bool unconfigured) {
    std::lock_guard<std::mutex> lock(mutex_);
    unconfigured_ = unconfigured;
}

void InkWeatherRenderer::SetSpeakCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    speak_callback_ = std::move(callback);
}

void InkWeatherRenderer::SetRefreshCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    refresh_callback_ = std::move(callback);
}

void InkWeatherRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    ink::WeatherData data;
    bool loading = false;
    bool failed = false;
    bool unconfigured = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data = data_;
        loading = loading_;
        failed = failed_;
        unconfigured = unconfigured_;
    }

    DrawRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, WHITE);
    if (data.valid) {
        RenderData(fb, width, height, data);
    } else {
        RenderEmpty(fb, width, height, loading, failed, unconfigured);
    }
    needs_full_refresh_ = false;
}

void InkWeatherRenderer::RenderEmpty(uint8_t* fb, int width, int height, bool loading, bool failed,
                                     bool unconfigured) const {
    const char* title = unconfigured ? "未配置天气凭据"
                        : loading    ? "正在获取天气..."
                        : failed     ? "天气获取失败"
                                     : "暂无天气数据";
    const char* hint = unconfigured ? "请在 sdkconfig 中设置和风天气凭据"
                       : loading    ? ""
                                    : "单击 BOOT 播报  双击刷新";
    const int center_x = width / 2;
    const int mid = Style::kStatusBarHeight + (height - Style::kStatusBarHeight) / 2;
    ink_text::DrawCenteredAtInkTop(fb, width, height, center_x, mid - 22, title, CityFont(), BLACK);
    ink_text::DrawCenteredAtInkTop(fb, width, height, center_x, mid + 6, hint, CityFont(), BLACK);
}

void InkWeatherRenderer::RenderData(uint8_t* fb, int width, int height, const ink::WeatherData& data) const {
    const std::vector<std::string> advice_lines =
        ink_text::Wrap(data.advice, Serif12(), width - kAdviceMarginX * 2, kAdviceMaxLines);
    const int advice_n = static_cast<int>(advice_lines.size());
    const int advice_h = advice_n <= 0 ? 0 : advice_n * kInfoLineSlot + (advice_n - 1) * kInfoLineGap;

    int divider_y = height - kAdviceGap;
    int advice_y = 0;
    if (advice_n > 0) {
        advice_y = height - kAdviceGap - advice_h;
        divider_y = advice_y - kAdviceGap;
    }
    if (divider_y <= Style::kStatusBarHeight) {
        divider_y = Style::kStatusBarHeight + 1;
    }

    const int content_h = LayoutBody(nullptr, width, height, 0, data);
    const int region_h = divider_y - Style::kStatusBarHeight;
    int y = Style::kStatusBarHeight;
    if (content_h < region_h) {
        y += (region_h - content_h) / 2;
    }

    LayoutBody(fb, width, height, y, data);
    DrawHLine(fb, width, divider_y, kSeparatorMarginX, width - kSeparatorMarginX, BLACK);

    int ay = advice_y;
    for (const auto& line : advice_lines) {
        ink_text::DrawCenteredAtInkTop(fb, width, height, width / 2, ay, line.c_str(), Serif12(), BLACK);
        ay += kInfoLineSlot + kInfoLineGap;
    }
}

int InkWeatherRenderer::LayoutBody(uint8_t* fb, int width, int height, int y,
                                   const ink::WeatherData& data) const {
    if (!data.city.empty()) {
        const int h = PlaceAtInkTop(fb, width, height, kCityMarginX, y, data.city.c_str(), CityFont(), BLACK);
        y += h + 6;
    }
    y += kCitySpacer;

    const int row_top = y;
    int left_y = row_top;
    {
        char temp[16];
        snprintf(temp, sizeof(temp), "%d°C", data.temp);
        const int h = PlaceAtInkTop(fb, width, height, kLeftX, left_y, temp, TempFont(), BLACK);
        left_y += h + 6;
        left_y += 2;

        constexpr int kIconSize = 24;
        constexpr int kIconGap = 4;
        DrawWeatherIcon(fb, width, kLeftX, left_y, data.code, kIconSize);
        const int text_x = kLeftX + (data.code >= 0 ? kIconSize + kIconGap : 0);
        const ink_text::InkBox desc_box = ink_text::Measure(Serif16(), data.desc.c_str());
        const int desc_top = left_y + (kIconSize - desc_box.height) / 2;
        PlaceAtInkTop(fb, width, height, text_x, desc_top, data.desc.c_str(), Serif16(), BLACK);
        left_y += kIconSize + 6;
        left_y += 4;

        char range[32];
        snprintf(range, sizeof(range), "%d°C / %d°C", data.low, data.high);
        const int rh = PlaceAtInkTop(fb, width, height, kLeftX, left_y, range, Serif14(), BLACK);
        left_y += rh;
    }

    const int right_x = kLeftX + kLeftWidth + kColumnGap;
    const int right_y = LayoutForecastCards(fb, width, height, right_x, width - right_x, row_top, data);
    y = (left_y > right_y ? left_y : right_y) + 6;

    PlaceAtInkTop(fb, width, height, kInfoMarginX, y, HumidityWindLine(data).c_str(), Serif12(), BLACK);
    y += kInfoLineSlot + kInfoLineGap;
    y += 4;

    if (!data.life.empty()) {
        const auto lines = ink_text::Wrap(data.life, Serif12(), width - kInfoMarginX * 2, 1);
        if (!lines.empty()) {
            PlaceAtInkTop(fb, width, height, kInfoMarginX, y, lines[0].c_str(), Serif12(), BLACK);
        }
        y += kInfoLineSlot + kInfoLineGap;
    }

    const std::string sun = SunLine(data);
    if (!sun.empty()) {
        PlaceAtInkTop(fb, width, height, kInfoMarginX, y, sun.c_str(), Serif12(), BLACK);
        y += kInfoLineSlot + kInfoLineGap;
    }

    return y + kBeforeDividerGap;
}

int InkWeatherRenderer::LayoutForecastCards(uint8_t* fb, int width, int height, int x, int avail_w, int top,
                                            const ink::WeatherData& data) const {
    const int n = data.forecast_count;
    if (n <= 0) return top;

    const int total_width = avail_w - kCardsMarginX * 2;
    int card_width = (total_width - kCardsGap * (n - 1)) / n;
    if (card_width < 40) card_width = 40;

    constexpr int kDayGap = 3;
    constexpr int kDateGap = 5;
    constexpr int kIconSize = 32;
    constexpr int kIconGap = 4;
    constexpr int kDescGap = 3;
    constexpr int kMarginBottom = 4;

    int bottom = top;
    for (int i = 0; i < n; ++i) {
        const ink::ForecastDay& day = data.forecast[i];
        const int x0 = x + kCardsMarginX + i * (card_width + kCardsGap);
        const int cx = x0 + card_width / 2;
        int y = top;

        y += PlaceCenteredAtInkTop(fb, width, height, cx, y, day.day.c_str(), Serif14(), BLACK) + kDayGap;
        y += PlaceCenteredAtInkTop(fb, width, height, cx, y, day.date.c_str(), Serif12(), BLACK) + kDateGap;
        if (day.code >= 0) {
            DrawWeatherIcon(fb, width, cx - kIconSize / 2, y, day.code, kIconSize);
            y += kIconSize + kIconGap;
        }
        const auto desc = ink_text::Wrap(day.desc, Serif12(), card_width, 1);
        const std::string desc_line = desc.empty() ? std::string() : desc[0];
        y += PlaceCenteredAtInkTop(fb, width, height, cx, y, desc_line.c_str(), Serif12(), BLACK) + kDescGap;
        char temp[24];
        snprintf(temp, sizeof(temp), "%d/%d°", day.temp_min, day.temp_max);
        y += PlaceCenteredAtInkTop(fb, width, height, cx, y, temp, Serif12(), BLACK);
        if (y > bottom) bottom = y;
    }
    return bottom + kMarginBottom;
}

bool InkWeatherRenderer::HandleInput(const ButtonEvent& event) {
    if (event.type == ButtonEvent::kBootClick) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = speak_callback_;
        }
        if (callback) callback();
        return false;
    }
    if (event.type != ButtonEvent::kBootDoubleClick) {
        return false;
    }
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = refresh_callback_;
        if (!data_.valid) {
            loading_ = true;
            failed_ = false;
        }
    }
    if (callback) callback();
    needs_full_refresh_ = true;
    return true;
}

}  // namespace rawdraw
