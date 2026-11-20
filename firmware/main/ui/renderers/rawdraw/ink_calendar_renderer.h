/**
 * @file ink_calendar_renderer.h
 * @brief Month calendar ported from InkSight's CALENDAR mode (calendar_grid block)
 *
 * Monday-first grid. The weekday bar reads 星期一–星期日: black Monday–Friday,
 * red Saturday and Sunday, white type. Today is a red rounded rectangle around
 * both the solar day and the lunar label. The tip under the dashed rule is
 * centered between that rule and the bottom edge. Labels prefer festival,
 * then solar term, then lunar day, and are chosen offline instead of by an LLM.
 *
 * BOOT opens a daily almanac (时辰宜忌) for today; BOOT again returns to the
 * month grid. UP/DOWN turn the day while that view is open.
 */

#ifndef INK_CALENDAR_RENDERER_H
#define INK_CALENDAR_RENDERER_H

#include "page_renderer.h"

#include <string>

namespace rawdraw {

class InkCalendarRenderer : public PageRenderer {
public:
    InkCalendarRenderer();

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /** @brief Status bar title: month name, or "老黄历" in the daily view */
    std::string GetTitle() const;

    /** @brief Show a specific month; used by host-side previews and tests */
    void SetMonth(int year, int month);

    struct DayLabel {
        const char* text = "";
        bool highlight = false;  // festival or solar term
    };
    static DayLabel LabelForDate(int year, int month, int day);

private:
    void JumpToToday();
    void ShiftDay(int delta);
    void RenderMonth(uint8_t* fb, int width, int height);
    void RenderDaily(uint8_t* fb, int width, int height) const;

    int year_ = 2026;
    int month_ = 1;
    int day_ = 1;
    bool daily_view_ = false;
};

}  // namespace rawdraw

#endif  // INK_CALENDAR_RENDERER_H
