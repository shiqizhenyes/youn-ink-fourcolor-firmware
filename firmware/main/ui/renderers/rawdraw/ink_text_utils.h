/**
 * @file ink_text_utils.h
 * @brief Text helpers shared by the InkSight-style pages
 *
 * InkSight lays text out with PIL bounding boxes, so these helpers position
 * text by its ink top instead of the font line box.
 */

#ifndef INK_TEXT_UTILS_H
#define INK_TEXT_UTILS_H

#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"

#include <string>
#include <vector>

namespace rawdraw {
namespace ink_text {

struct InkBox {
    int top = 0;     // offset from DrawText y to the first ink row
    int height = 0;  // ink rows
};

inline InkBox Measure(const lv_font_t* font, const char* text) {
    const TextInkBounds b = MeasureTextInkBounds(font, text);
    if (!b.valid) return {0, font ? font->line_height : 0};
    return {b.top, b.height};
}

// Draws text so its ink starts at ink_top; returns the ink height.
inline int DrawAtInkTop(uint8_t* fb, int width, int height, int x, int ink_top,
                        const char* text, const lv_font_t* font, Color color) {
    if (!text || !*text) return 0;
    const InkBox box = Measure(font, text);
    DrawText(fb, width, x, ink_top - box.top, text, font, color, height);
    return box.height;
}

inline int DrawCenteredAtInkTop(uint8_t* fb, int width, int height, int center_x, int ink_top,
                                const char* text, const lv_font_t* font, Color color) {
    if (!text || !*text) return 0;
    const int w = MeasureTextWidth(text, font);
    return DrawAtInkTop(fb, width, height, center_x - w / 2, ink_top, text, font, color);
}

// Greedy per-character wrap (CJK text has no word boundaries).
inline std::vector<std::string> Wrap(const std::string& text, const lv_font_t* font,
                                     int max_width, int max_lines) {
    std::vector<std::string> lines;
    std::string current;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        utf8_next(&p);
        std::string candidate = current;
        candidate.append(start, p - start);
        if (!current.empty() && MeasureTextWidth(candidate.c_str(), font) > max_width) {
            lines.push_back(current);
            current.assign(start, p - start);
            if (static_cast<int>(lines.size()) == max_lines) {
                std::string& last = lines.back();
                while (!last.empty() && MeasureTextWidth((last + "...").c_str(), font) > max_width) {
                    size_t cut = last.size() - 1;
                    while (cut > 0 && (static_cast<unsigned char>(last[cut]) & 0xC0) == 0x80) --cut;
                    last.resize(cut);
                }
                last += "...";
                return lines;
            }
        } else {
            current = std::move(candidate);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

}  // namespace ink_text
}  // namespace rawdraw

#endif  // INK_TEXT_UTILS_H
