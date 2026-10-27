#include "ui.hpp"

#include "ui_font_bin.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace MoonlightUi {
namespace {

constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 240;
constexpr Color COLOR_ACCENT_RGB = {72, 229, 190};
constexpr Color COLOR_SELECTED_RGB = {72, 235, 197};
constexpr Color COLOR_WHITE = {242, 247, 250};
constexpr Color COLOR_MUTED = {119, 144, 167};
constexpr Color COLOR_TEAL = {113, 240, 207};
constexpr Color COLOR_FOOTER = {145, 165, 184};

constexpr size_t GLYPH_RECORD_SIZE = 13;
constexpr int KERNED_CHARACTERS = 95;
constexpr uint8_t FONT_SMALL = 0;
constexpr uint8_t FONT_HEADER = 1;
constexpr uint8_t FONT_TILE = 2;
constexpr uint8_t FONT_RATE_15 = 3;
constexpr uint8_t FONT_RATE_14 = 4;
constexpr uint8_t FONT_RATE_13 = 5;
constexpr uint8_t FONT_RATE_12 = 6;

const uint8_t *const FONT_DATA = ui_font_bin;

uint16_t read_u16(size_t offset) {
    return static_cast<uint16_t>(FONT_DATA[offset]) |
           (static_cast<uint16_t>(FONT_DATA[offset + 1]) << 8);
}

uint32_t read_u32(size_t offset) {
    return static_cast<uint32_t>(FONT_DATA[offset]) |
           (static_cast<uint32_t>(FONT_DATA[offset + 1]) << 8) |
           (static_cast<uint32_t>(FONT_DATA[offset + 2]) << 16) |
           (static_cast<uint32_t>(FONT_DATA[offset + 3]) << 24);
}

uint16_t font_count() { return read_u16(6); }
uint16_t character_count() { return read_u16(8); }
uint32_t charset_offset() { return read_u32(16); }
uint32_t glyph_table_offset() { return read_u32(20); }
uint32_t kerning_offset() { return read_u32(24); }

int character_index(uint32_t codepoint) {
    const int count = character_count();
    int low = 0;
    int high = count - 1;
    while (low <= high) {
        const int middle = low + (high - low) / 2;
        const uint32_t candidate =
            read_u32(charset_offset() + static_cast<size_t>(middle) * 4);
        if (candidate == codepoint) {
            return middle;
        }
        if (candidate < codepoint) {
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }
    return -1;
}

struct Glyph {
    int x_offset;
    int y_offset;
    int width;
    int height;
    int advance_64;
    size_t pixels;
};

Glyph glyph_for(uint8_t font_id, int char_index) {
    const size_t record = glyph_table_offset() +
                          (static_cast<size_t>(font_id) * character_count() +
                           static_cast<size_t>(char_index)) *
                              GLYPH_RECORD_SIZE;
    return {static_cast<int8_t>(FONT_DATA[record + 3]),
            static_cast<int8_t>(FONT_DATA[record + 4]), FONT_DATA[record + 5],
            FONT_DATA[record + 6], read_u16(record + 7),
            read_u32(record + 9)};
}

int8_t kerning_adjustment(uint8_t font_id, int left_index, int right_index) {
    if (left_index >= KERNED_CHARACTERS ||
        right_index >= KERNED_CHARACTERS) {
        return 0;
    }
    const size_t pair =
        (static_cast<size_t>(font_id) * KERNED_CHARACTERS +
         static_cast<size_t>(left_index)) *
            KERNED_CHARACTERS +
        static_cast<size_t>(right_index);
    return static_cast<int8_t>(FONT_DATA[kerning_offset() + pair]);
}

uint32_t next_codepoint(const unsigned char **text,
                        const unsigned char *end) {
    const unsigned char *cursor = *text;
    if (cursor >= end) {
        return 0;
    }
    if (*cursor < 0x80) {
        *text = cursor + 1;
        return *cursor;
    }
    if ((*cursor & 0xE0) == 0xC0 && cursor + 1 < end &&
        (cursor[1] & 0xC0) == 0x80) {
        *text = cursor + 2;
        return (static_cast<uint32_t>(cursor[0] & 0x1F) << 6) |
               static_cast<uint32_t>(cursor[1] & 0x3F);
    }
    if ((*cursor & 0xF0) == 0xE0 && cursor + 2 < end &&
        (cursor[1] & 0xC0) == 0x80 &&
        (cursor[2] & 0xC0) == 0x80) {
        *text = cursor + 3;
        return (static_cast<uint32_t>(cursor[0] & 0x0F) << 12) |
               (static_cast<uint32_t>(cursor[1] & 0x3F) << 6) |
               static_cast<uint32_t>(cursor[2] & 0x3F);
    }
    if ((*cursor & 0xF8) == 0xF0 && cursor + 3 < end &&
        (cursor[1] & 0xC0) == 0x80 &&
        (cursor[2] & 0xC0) == 0x80 && (cursor[3] & 0xC0) == 0x80) {
        *text = cursor + 4;
        return (static_cast<uint32_t>(cursor[0] & 0x07) << 18) |
               (static_cast<uint32_t>(cursor[1] & 0x3F) << 12) |
               (static_cast<uint32_t>(cursor[2] & 0x3F) << 6) |
               static_cast<uint32_t>(cursor[3] & 0x3F);
    }
    *text = cursor + 1;
    return '?';
}

int resolved_character_index(uint32_t codepoint) {
    int index = character_index(codepoint);
    if (index < 0) {
        index = character_index('?');
    }
    return index;
}

int measure_text(const std::string &text, uint8_t font_id) {
    if (FONT_DATA[0] != 'M' || FONT_DATA[1] != 'L' || FONT_DATA[2] != 'G' ||
        FONT_DATA[3] != 'F' || font_id >= font_count()) {
        return 0;
    }
    int width_64 = 0;
    int previous_char = -1;
    const unsigned char *cursor =
        reinterpret_cast<const unsigned char *>(text.c_str());
    const unsigned char *end = cursor + text.size();
    while (cursor < end) {
        const uint32_t codepoint = next_codepoint(&cursor, end);
        if (codepoint == '\n') {
            break;
        }
        if (codepoint == '\r') {
            continue;
        }
        const int index = resolved_character_index(codepoint);
        if (index < 0) {
            continue;
        }
        if (previous_char >= 0) {
            width_64 += static_cast<int>(kerning_adjustment(
                             font_id, previous_char, index)) *
                        4;
        }
        width_64 += glyph_for(font_id, index).advance_64;
        previous_char = index;
    }
    return (width_64 + 32) / 64;
}

int font_pixel_size(uint8_t font_id) {
    switch (font_id) {
    case FONT_SMALL:
        return 10;
    case FONT_HEADER:
        return 17;
    case FONT_TILE:
        return 20;
    case FONT_RATE_15:
        return 15;
    case FONT_RATE_14:
        return 14;
    case FONT_RATE_13:
        return 13;
    default:
        return 12;
    }
}

uint16_t rgb565(Color color) {
    return static_cast<uint16_t>(((color.red >> 3) << 11) |
                                 ((color.green >> 2) << 5) |
                                 (color.blue >> 3));
}

int blend_channel(int background, int foreground, unsigned int alpha,
                  unsigned int denominator) {
    return (background * static_cast<int>(denominator - alpha) +
            foreground * static_cast<int>(alpha) +
            static_cast<int>(denominator / 2)) /
           static_cast<int>(denominator);
}

bool inside_rounded_rect(const Rect &rect, int radius, int x, int y) {
    if (x < rect.left || x > rect.right || y < rect.top || y > rect.bottom) {
        return false;
    }
    const int center_x = std::max(rect.left + radius,
                                  std::min(x, rect.right - radius));
    const int center_y = std::max(rect.top + radius,
                                  std::min(y, rect.bottom - radius));
    const int delta_x = x - center_x;
    const int delta_y = y - center_y;
    return delta_x * delta_x + delta_y * delta_y <= radius * radius;
}

bool poll_input(InputEvent *event) {
    hidScanInput();
    const u32 keys = hidKeysDown();
    if (keys & KEY_B) {
        event->type = InputEvent::Type::Back;
        return true;
    }
    if (keys & KEY_A) {
        event->type = InputEvent::Type::Confirm;
        return true;
    }
    if (keys & KEY_TOUCH) {
        event->type = InputEvent::Type::Touch;
        hidTouchRead(&event->touch);
        return true;
    }

    const u32 left = KEY_LEFT | KEY_CPAD_LEFT;
    const u32 right = KEY_RIGHT | KEY_CPAD_RIGHT;
    const u32 up = KEY_UP | KEY_CPAD_UP;
    const u32 down = KEY_DOWN | KEY_CPAD_DOWN;
    if (keys & up) {
        event->type = InputEvent::Type::Direction;
        event->direction = Direction::Up;
        return true;
    }
    if (keys & down) {
        event->type = InputEvent::Type::Direction;
        event->direction = Direction::Down;
        return true;
    }
    if (keys & left) {
        event->type = InputEvent::Type::Direction;
        event->direction = Direction::Left;
        return true;
    }
    if (keys & right) {
        event->type = InputEvent::Type::Direction;
        event->direction = Direction::Right;
        return true;
    }
    return false;
}

} // namespace

Screen::Screen() : framebuffer_(nullptr) {
    // Bottom UI screens render into a hidden back buffer and commit it at VBlank.
    // Stream renderers remain single-buffered after this session ends.
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
}

Screen::~Screen() {
    gspWaitForVBlank();
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
}

void Screen::setPixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT ||
        framebuffer_ == nullptr) {
        return;
    }
    // Keep the ui2 framebuffer mapping: logical (x,y) maps to
    // framebuffer[x * 240 + (239 - y)].
    framebuffer_[x * GSP_SCREEN_WIDTH + (GSP_SCREEN_WIDTH - 1 - y)] = color;
}

void Screen::blendPixel(int x, int y, Color color, unsigned int coverage) {
    if (coverage == 0 || x < 0 || x >= SCREEN_WIDTH || y < 0 ||
        y >= SCREEN_HEIGHT || framebuffer_ == nullptr) {
        return;
    }
    coverage = std::min(coverage, 15U);
    const size_t offset =
        static_cast<size_t>(x) * GSP_SCREEN_WIDTH +
        static_cast<size_t>(GSP_SCREEN_WIDTH - 1 - y);
    const uint16_t pixel = framebuffer_[offset];
    const int old_red = ((pixel >> 11) & 0x1F) * 255 / 31;
    const int old_green = ((pixel >> 5) & 0x3F) * 255 / 63;
    const int old_blue = (pixel & 0x1F) * 255 / 31;
    Color mixed = {
        static_cast<uint8_t>(blend_channel(old_red, color.red, coverage, 15)),
        static_cast<uint8_t>(blend_channel(old_green, color.green, coverage, 15)),
        static_cast<uint8_t>(blend_channel(old_blue, color.blue, coverage, 15)),
    };
    framebuffer_[offset] = rgb565(mixed);
}

void Screen::fillRoundedRect(const Rect &rect, int radius, uint16_t color) {
    const int left = std::max(0, rect.left);
    const int top = std::max(0, rect.top);
    const int right = std::min(SCREEN_WIDTH - 1, rect.right);
    const int bottom = std::min(SCREEN_HEIGHT - 1, rect.bottom);
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            if (inside_rounded_rect(rect, radius, x, y)) {
                setPixel(x, y, color);
            }
        }
    }
}

void Screen::drawBackground() {
    framebuffer_ = reinterpret_cast<uint16_t *>(
        gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr));
    if (framebuffer_ == nullptr) {
        return;
    }

    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        const Color row = {
            static_cast<uint8_t>(8 + (6 * y + 119) / 239),
            static_cast<uint8_t>(17 + (10 * y + 119) / 239),
            static_cast<uint8_t>(30 + (13 * y + 119) / 239),
        };
        const uint16_t color = rgb565(row);
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            setPixel(x, y, color);
        }
    }

    // The two translucent corner ellipses from the ui2 shared background.
    for (int y = 0; y <= 76; ++y) {
        for (int x = 236; x < SCREEN_WIDTH; ++x) {
            const int dx = x - 313;
            const int dy = y + 1;
            if (dx * dx + dy * dy <= 77 * 77) {
                const int base_red = 8 + (6 * y + 119) / 239;
                const int base_green = 17 + (10 * y + 119) / 239;
                const int base_blue = 30 + (13 * y + 119) / 239;
                Color over = {
                    static_cast<uint8_t>(blend_channel(base_red, 47, 20, 255)),
                    static_cast<uint8_t>(blend_channel(base_green, 116, 20, 255)),
                    static_cast<uint8_t>(blend_channel(base_blue, 113, 20, 255)),
                };
                setPixel(x, y, rgb565(over));
            }
        }
    }
    for (int y = 176; y < SCREEN_HEIGHT; ++y) {
        for (int x = 0; x <= 94; ++x) {
            const int dx = x + 5;
            const int dy = y - 275;
            if (dx * dx + dy * dy <= 100 * 100) {
                const int base_red = 8 + (6 * y + 119) / 239;
                const int base_green = 17 + (10 * y + 119) / 239;
                const int base_blue = 30 + (13 * y + 119) / 239;
                Color over = {
                    static_cast<uint8_t>(blend_channel(base_red, 33, 18, 255)),
                    static_cast<uint8_t>(blend_channel(base_green, 75, 18, 255)),
                    static_cast<uint8_t>(blend_channel(base_blue, 110, 18, 255)),
                };
                setPixel(x, y, rgb565(over));
            }
        }
    }

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        int header_red = 8 + (6 * 60 + 119) / 239;
        int header_green = 17 + (10 * 60 + 119) / 239;
        int header_blue = 30 + (13 * 60 + 119) / 239;
        const int header_dx = x - 313;
        const int header_dy = 61;
        if (header_dx * header_dx + header_dy * header_dy <= 77 * 77) {
            header_red = blend_channel(header_red, 47, 20, 255);
            header_green = blend_channel(header_green, 116, 20, 255);
            header_blue = blend_channel(header_blue, 113, 20, 255);
        }
        const Color header_line = {
            static_cast<uint8_t>(blend_channel(header_red, 87, 65, 255)),
            static_cast<uint8_t>(blend_channel(header_green, 123, 65, 255)),
            static_cast<uint8_t>(blend_channel(header_blue, 143, 65, 255)),
        };
        setPixel(x, 60, rgb565(header_line));

        const int y = 222;
        int footer_red = 8 + (6 * y + 119) / 239;
        int footer_green = 17 + (10 * y + 119) / 239;
        int footer_blue = 30 + (13 * y + 119) / 239;
        if (x <= 94) {
            const int dx = x + 5;
            const int dy = y - 275;
            if (dx * dx + dy * dy <= 100 * 100) {
                footer_red = blend_channel(footer_red, 33, 18, 255);
                footer_green = blend_channel(footer_green, 75, 18, 255);
                footer_blue = blend_channel(footer_blue, 110, 18, 255);
            }
        }
        const Color footer_line = {
            static_cast<uint8_t>(blend_channel(footer_red, 87, 45, 255)),
            static_cast<uint8_t>(blend_channel(footer_green, 123, 45, 255)),
            static_cast<uint8_t>(blend_channel(footer_blue, 143, 45, 255)),
        };
        setPixel(x, y, rgb565(footer_line));
    }
}

void Screen::drawTextStyle(int x, int y, const std::string &text, int font_id,
                           Color color) {
    if (framebuffer_ == nullptr || FONT_DATA[0] != 'M' || FONT_DATA[1] != 'L' ||
        FONT_DATA[2] != 'G' || FONT_DATA[3] != 'F' || font_id < 0 ||
        font_id >= font_count()) {
        return;
    }

    int pen_x_64 = 0;
    int line_y = y;
    int previous_char = -1;
    const unsigned char *cursor =
        reinterpret_cast<const unsigned char *>(text.c_str());
    const unsigned char *end = cursor + text.size();
    while (cursor < end) {
        const uint32_t codepoint = next_codepoint(&cursor, end);
        if (codepoint == '\n') {
            pen_x_64 = 0;
            line_y += font_pixel_size(static_cast<uint8_t>(font_id));
            previous_char = -1;
            continue;
        }
        if (codepoint == '\r') {
            continue;
        }
        int index = resolved_character_index(codepoint);
        if (index < 0) {
            continue;
        }
        if (previous_char >= 0) {
            pen_x_64 += static_cast<int>(kerning_adjustment(
                             static_cast<uint8_t>(font_id), previous_char,
                             index)) *
                        4;
        }
        const Glyph glyph = glyph_for(static_cast<uint8_t>(font_id), index);
        const int fractional_x = pen_x_64 & 63;
        const int origin_x = x + (pen_x_64 >> 6) + glyph.x_offset;
        const int origin_y = line_y + glyph.y_offset;
        for (int gy = 0; gy < glyph.height; ++gy) {
            for (int gx = 0; gx < glyph.width; ++gx) {
                const size_t pixel_index =
                    static_cast<size_t>(gy) * glyph.width + gx;
                const uint8_t packed = FONT_DATA[
                    glyph.pixels + pixel_index / 2];
                const unsigned int coverage =
                    (pixel_index & 1) ? packed & 0x0F : packed >> 4;
                if (coverage == 0) {
                    continue;
                }
                const unsigned int left_coverage =
                    coverage * static_cast<unsigned int>(64 - fractional_x) /
                    64;
                const unsigned int right_coverage = coverage - left_coverage;
                blendPixel(origin_x + gx, origin_y + gy, color,
                           left_coverage);
                blendPixel(origin_x + gx + 1, origin_y + gy, color,
                           right_coverage);
            }
        }
        pen_x_64 += glyph.advance_64;
        previous_char = index;
    }
}

void Screen::drawFilledTile(const Rect &rect, int number) {
    const uint16_t border = rgb565({41, 64, 90});
    const uint16_t fill = rgb565({19, 34, 56});
    fillRoundedRect(rect, 10, border);
    Rect inner = {rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1};
    fillRoundedRect(inner, 9, fill);
    Rect badge = {rect.right - 18, rect.top + 11, rect.right - 9,
                  rect.top + 20};
    fillRoundedRect(badge, 4, rgb565({31, 60, 80}));

    char number_text[3];
    number_text[0] = static_cast<char>('0' + ((number / 10) % 10));
    number_text[1] = static_cast<char>('0' + (number % 10));
    number_text[2] = '\0';
    drawTextStyle(rect.left + 9, rect.top + 7, number_text, FONT_SMALL,
                  COLOR_MUTED);
}

void Screen::drawListRow(const Rect &rect, int number,
                         const std::string &label, bool selected) {
    drawFilledTile(rect, number);
    drawLiveText(rect.left + 28, rect.top + (rect.bottom - rect.top - 17) / 2,
                 label, TextStyle::Header, COLOR_WHITE);
    if (selected) {
        drawHighlight(rect);
    }
}

void Screen::drawHeader(const std::string &title,
                        const std::string &context) {
    fillRoundedRect({14, 11, 18, 48}, 2, rgb565(COLOR_ACCENT_RGB));
    drawLiveText(25, 10, title, TextStyle::Small, COLOR_TEAL);
    drawLiveText(25, 25, context, TextStyle::Header, COLOR_WHITE);
}

void Screen::drawFooter(const std::string &version, const std::string &hint) {
    const std::string footer =
        "Moonlight " + version + "  ·  " + hint;
    drawLiveText(14, 226, footer, TextStyle::Small, COLOR_FOOTER);
}

void Screen::drawHighlight(const Rect &rect) {
    const uint16_t color = rgb565(COLOR_SELECTED_RGB);
    for (int thickness = 0; thickness < 3; ++thickness) {
        const int left = rect.left + thickness;
        const int right = rect.right - thickness;
        const int top = rect.top + thickness;
        const int bottom = rect.bottom - thickness;
        for (int x = left; x <= right; ++x) {
            setPixel(x, top, color);
            setPixel(x, bottom, color);
        }
        for (int y = top; y <= bottom; ++y) {
            setPixel(left, y, color);
            setPixel(right, y, color);
        }
    }
}

void Screen::drawLiveText(int x, int y, const std::string &text,
                          TextStyle style, Color color) {
    const int font_id = style == TextStyle::Small
                            ? FONT_SMALL
                            : (style == TextStyle::Header ? FONT_HEADER
                                                          : FONT_TILE);
    drawTextStyle(x, y, text, font_id, color);
}

int Screen::measureLiveText(const std::string &text, TextStyle style) const {
    const int font_id = style == TextStyle::Small
                            ? FONT_SMALL
                            : (style == TextStyle::Header ? FONT_HEADER
                                                          : FONT_TILE);
    return measure_text(text, static_cast<uint8_t>(font_id));
}

void Screen::drawAutoRateText(int x, int y, const std::string &text,
                              int max_width, Color color) {
    int font_id = FONT_RATE_15;
    for (; font_id <= FONT_RATE_12; ++font_id) {
        if (measure_text(text, static_cast<uint8_t>(font_id)) <= max_width ||
            font_id == FONT_RATE_12) {
            break;
        }
    }
    drawTextStyle(x, y, text, font_id, color);
}

void Screen::present() {
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    gspWaitForVBlank();
}

bool runScreen(ScreenModel &model) {
    Screen screen;
    bool redraw = true;
    while (aptMainLoop()) {
        if (redraw) {
            screen.drawBackground();
            model.draw(screen);
            screen.present();
            redraw = false;
        }

        InputEvent event{};
        if (!poll_input(&event)) {
            gspWaitForVBlank();
            continue;
        }
        if (!model.handleInput(event, &redraw)) {
            return true;
        }
    }
    return false;
}

} // namespace MoonlightUi
