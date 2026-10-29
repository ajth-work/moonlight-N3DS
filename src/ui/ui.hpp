#pragma once

#include <3ds.h>

#include <cstdint>
#include <string>
#include <vector>

namespace MoonlightUi {

struct Rect {
    int left;
    int top;
    int right;
    int bottom;
};

struct Color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

enum class TextStyle {
    Small,
    Header,
    Tile,
};

enum class Direction {
    Up,
    Down,
    Left,
    Right,
};

struct InputEvent {
    enum class Type {
        Direction,
        Confirm,
        Back,
        Touch,
    } type;
    Direction direction;
    touchPosition touch;
};

class Screen {
  public:
    explicit Screen(gfxScreen_t display = GFX_BOTTOM);
    ~Screen();

    Screen(const Screen &) = delete;
    Screen &operator=(const Screen &) = delete;

    int width() const;
    int height() const;
    void drawBackground();
    void drawSolidBackground(Color color);
    void drawCircle(int center_x, int center_y, int radius, Color color);
    void drawMagnifierIcon(int center_x, int center_y, Color color);
    void drawKeyboardIcon(int center_x, int center_y, Color color);
    void drawSettingsIcon(int center_x, int center_y, Color color);
    void drawPanelButton(const Rect &rect, const std::string &label,
                         Color fill, Color text);
    void drawFilledTile(const Rect &rect, int number);
    void drawProgressBar(const Rect &rect, unsigned int percent, Color color);
    void drawListRow(const Rect &rect, int number, const std::string &label,
                     bool selected);
    void drawHeader(const std::string &title, const std::string &context);
    void drawFooter(const std::string &version, const std::string &hint);
    void drawHighlight(const Rect &rect);
    void drawFrameTimingGraph(const Rect &rect,
                              const std::vector<uint16_t> &intervals_ms,
                              uint16_t scale_ms, Color color);
    void drawDisplaySelector(int selected, int count, bool configure_count);
    void drawLiveText(int x, int y, const std::string &text, TextStyle style,
                      Color color);
    void drawAutoRateText(int x, int y, const std::string &text,
                          int max_width, Color color);
    int measureLiveText(const std::string &text, TextStyle style) const;
    void present(bool wait_for_vblank = true);

  private:
    gfxScreen_t display_;
    int width_;
    int height_;
    uint16_t *framebuffer_;
    bool double_buffering_enabled_;

    void setPixel(int x, int y, uint16_t color);
    void blendPixel(int x, int y, Color color, unsigned int coverage);
    void fillRoundedRect(const Rect &rect, int radius, uint16_t color);
    void drawTextStyle(int x, int y, const std::string &text, int font_id,
                       Color color);
};

class ScreenModel {
  public:
    virtual ~ScreenModel() = default;
    virtual void draw(Screen &screen) = 0;
    // Called once per frame; return false to dismiss the screen.
    virtual bool update(bool *redraw) {
        *redraw = false;
        return true;
    }
    // Return false to leave the screen. Set redraw when visible state changed.
    virtual bool handleInput(const InputEvent &event, bool *redraw) = 0;
};

// Owns input polling and full-frame redraws for toolkit screens. Returns false
// only when the application loop ends before the screen is dismissed.
bool runScreen(ScreenModel &model);

class DualScreenModel {
  public:
    virtual ~DualScreenModel() = default;
    virtual void draw(Screen &top, Screen &bottom) = 0;
    // Return false to leave the screen. Set redraw when visible state changed.
    virtual bool handleInput(const InputEvent &event, bool *redraw) = 0;
};

// Draws a coordinated top/bottom screen UI while sharing one input model.
bool runDualScreen(DualScreenModel &model);

} // namespace MoonlightUi
