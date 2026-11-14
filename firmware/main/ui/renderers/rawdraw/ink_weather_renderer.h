/**
 * @file ink_weather_renderer.h
 * @brief Weather page ported from InkSight's WEATHER mode (weather.json layout)
 *
 * The page body starts below the firmware's own status bar; InkSight's
 * status bar and footer are intentionally not drawn.
 */

#ifndef INK_WEATHER_RENDERER_H
#define INK_WEATHER_RENDERER_H

#include "page_renderer.h"
#include "common/ink_weather_data.h"

#include <functional>
#include <mutex>

namespace rawdraw {

class InkWeatherRenderer : public PageRenderer {
public:
    InkWeatherRenderer();

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    void SetData(const ink::WeatherData& data);
    bool HasData() const;
    void SetLoading(bool loading);
    void SetFailed(bool failed);
    /** @brief Show "no weather keys" instead of fetching (keys missing from sdkconfig) */
    void SetUnconfigured(bool unconfigured);
    void SetSpeakCallback(std::function<void()> callback);
    void SetRefreshCallback(std::function<void()> callback);

private:
    void RenderEmpty(uint8_t* fb, int width, int height, bool loading, bool failed, bool unconfigured) const;
    void RenderData(uint8_t* fb, int width, int height, const ink::WeatherData& data) const;
    int LayoutForecastCards(uint8_t* fb, int width, int height, int x, int avail_w, int top,
                            const ink::WeatherData& data) const;
    int LayoutBody(uint8_t* fb, int width, int height, int y, const ink::WeatherData& data) const;

    mutable std::mutex mutex_;
    ink::WeatherData data_;
    bool loading_ = false;
    bool failed_ = false;
    bool unconfigured_ = false;
    std::function<void()> speak_callback_;
    std::function<void()> refresh_callback_;
};

}  // namespace rawdraw

#endif  // INK_WEATHER_RENDERER_H
