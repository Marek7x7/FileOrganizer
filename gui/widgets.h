#pragma once

// Minimal hand-rolled immediate-mode widget set for the SDL3 GUI. No
// retained widget tree, no generic ID stack -- callers pass a stable
// caller-defined int id for anything that needs to remember focus across
// frames (currently just text fields), and everything else is derived
// fresh from ctx's per-frame input snapshot each call.

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <deque>
#include <string>
#include <vector>

namespace ui {

struct Color { Uint8 r, g, b, a; };

constexpr Color kBg        {30, 32, 36, 255};
constexpr Color kPanel     {42, 45, 50, 255};
constexpr Color kPanelLite {52, 56, 62, 255};
constexpr Color kBorder    {70, 74, 82, 255};
constexpr Color kBorderHi  {110, 150, 220, 255};
constexpr Color kText      {225, 227, 230, 255};
constexpr Color kTextDim   {150, 154, 162, 255};
constexpr Color kAccent    {80, 130, 220, 255};
constexpr Color kAccentDim {60, 95, 160, 255};
constexpr Color kError     {230, 110, 110, 255};
constexpr Color kSuccess   {110, 195, 130, 255};

struct Rect { float x, y, w, h; };

bool pointIn(const Rect& r, float x, float y);

// Per-frame input snapshot plus shared rendering resources. Call
// beginFrame() once per frame (after feeding SDL events to it via the
// on*Event methods), lay out widgets in order, then endFrame() to present.
struct Context {
    SDL_Renderer* renderer = nullptr;
    TTF_Font* font = nullptr;
    int fontHeight = 14;

    float mouseX = 0, mouseY = 0;
    bool mouseDown = false;      // currently held
    bool mouseClicked = false;   // button-up occurred this frame
    float wheelY = 0;

    std::string textInput;       // UTF-8 text typed this frame
    bool keyBackspace = false;
    bool keyPaste = false;       // Ctrl+V
    bool keyCopy = false;        // Ctrl+C

    // Logical id (caller-defined, e.g. an enum value) of the text field
    // that currently has keyboard focus, or -1 for none.
    int focusedField = -1;

    void onMouseMotion(float x, float y);
    void onMouseButton(bool down, float x, float y);
    void onMouseWheel(float y);
    void onTextInput(const char* text);
    void onKeyDown(SDL_Keycode key, SDL_Keymod mod);
    void beginFrame();
    void endFrame();
};

void drawRect(Context& ctx, const Rect& r, Color fill, Color border);
void drawText(Context& ctx, float x, float y, const std::string& text, Color color);
float textWidth(Context& ctx, const std::string& text);

bool button(Context& ctx, const Rect& r, const std::string& label, bool enabled = true);
bool checkbox(Context& ctx, const Rect& r, const std::string& label, bool& value);
// Draws labels.size() inline radio buttons starting at r.x,r.y (r.h tall
// each); returns true if `selected` changed this frame.
bool radioGroup(Context& ctx, const Rect& r, const std::vector<std::string>& labels, int& selected);

// A single-line editable text field, append/backspace only (no mid-string
// cursor movement -- adequate for typing/pasting paths and short values).
// fieldId must be stable and unique among fields visible at once; it's how
// focus is tracked across frames. Returns true if buffer changed.
bool textField(Context& ctx, const Rect& r, int fieldId, std::string& buffer,
               const std::string& placeholder = "");

// A text field plus a "Browse..." button opening a native folder/file
// picker (SDL_ShowOpenFolderDialog / SDL_ShowOpenFileDialog). The dialog
// callback may run on another thread; call this every frame so a pending
// pick gets applied to `buffer` on the main thread.
bool pathField(Context& ctx, const Rect& r, int fieldId, std::string& buffer,
               SDL_Window* window, bool pickFolder);

void progressBar(Context& ctx, const Rect& r, double fraction);

// Draws the visible slice of `lines` inside r, newest entry at the bottom,
// with mouse-wheel scrolling. `scrollOffset` is lines-from-bottom (0 =
// pinned to newest) and persists across frames in caller-owned state.
// Takes a deque (rather than a vector) so the caller's ring-buffered log
// state can be passed straight through without a per-frame copy.
void logPanel(Context& ctx, const Rect& r, const std::deque<std::string>& lines, int& scrollOffset);

} // namespace ui
