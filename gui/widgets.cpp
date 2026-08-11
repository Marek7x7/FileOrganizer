#include "widgets.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace ui {

bool pointIn(const Rect& r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void Context::onMouseMotion(float x, float y) { mouseX = x; mouseY = y; }

void Context::onMouseButton(bool down, float x, float y) {
    mouseX = x; mouseY = y;
    mouseDown = down;
    if (!down) mouseClicked = true;
}

void Context::onMouseWheel(float y) { wheelY += y; }

void Context::onTextInput(const char* text) { textInput += text; }

void Context::onKeyDown(SDL_Keycode key, SDL_Keymod mod) {
    if (key == SDLK_BACKSPACE) keyBackspace = true;
    bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
    if (ctrl && key == SDLK_V) keyPaste = true;
    if (ctrl && key == SDLK_C) keyCopy = true;
}

void Context::beginFrame() {
    mouseClicked = false;
    wheelY = 0;
    textInput.clear();
    keyBackspace = false;
    keyPaste = false;
    keyCopy = false;
}

void Context::endFrame() {
    SDL_RenderPresent(renderer);
}

// -------------------- DRAWING PRIMITIVES --------------------

void drawRect(Context& ctx, const Rect& r, Color fill, Color border) {
    SDL_FRect fr{r.x, r.y, r.w, r.h};
    if (fill.a > 0) {
        SDL_SetRenderDrawColor(ctx.renderer, fill.r, fill.g, fill.b, fill.a);
        SDL_RenderFillRect(ctx.renderer, &fr);
    }
    if (border.a > 0) {
        SDL_SetRenderDrawColor(ctx.renderer, border.r, border.g, border.b, border.a);
        SDL_RenderRect(ctx.renderer, &fr);
    }
}

void drawText(Context& ctx, float x, float y, const std::string& text, Color color) {
    if (text.empty()) return;
    SDL_Color c{color.r, color.g, color.b, color.a};
    SDL_Surface* surf = TTF_RenderText_Blended(ctx.font, text.c_str(), text.size(), c);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ctx.renderer, surf);
    if (tex) {
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        SDL_FRect dst{x, y, (float)surf->w, (float)surf->h};
        SDL_RenderTexture(ctx.renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_DestroySurface(surf);
}

float textWidth(Context& ctx, const std::string& text) {
    if (text.empty()) return 0;
    int w = 0, h = 0;
    TTF_GetStringSize(ctx.font, text.c_str(), text.size(), &w, &h);
    return (float)w;
}

// -------------------- BUTTON / CHECKBOX / RADIO --------------------

bool button(Context& ctx, const Rect& r, const std::string& label, bool enabled) {
    bool hover = enabled && pointIn(r, ctx.mouseX, ctx.mouseY);
    bool clicked = enabled && hover && ctx.mouseClicked;

    Color fill = !enabled ? kPanel : (hover ? kAccentDim : kPanelLite);
    Color border = !enabled ? kBorder : (hover ? kBorderHi : kBorder);
    drawRect(ctx, r, fill, border);

    Color textColor = enabled ? kText : kTextDim;
    float tw = textWidth(ctx, label);
    drawText(ctx, r.x + (r.w - tw) / 2.0f, r.y + (r.h - ctx.fontHeight) / 2.0f, label, textColor);

    return clicked;
}

bool checkbox(Context& ctx, const Rect& r, const std::string& label, bool& value) {
    float boxSize = r.h;
    Rect box{r.x, r.y, boxSize, boxSize};
    bool hover = pointIn(box, ctx.mouseX, ctx.mouseY) ||
                 pointIn(Rect{r.x, r.y, r.w, r.h}, ctx.mouseX, ctx.mouseY);
    bool clicked = hover && ctx.mouseClicked;
    bool changed = false;
    if (clicked) { value = !value; changed = true; }

    drawRect(ctx, box, value ? kAccent : kPanelLite, hover ? kBorderHi : kBorder);
    if (value) {
        Rect inner{box.x + 4, box.y + 4, box.w - 8, box.h - 8};
        drawRect(ctx, inner, kText, kText);
    }
    drawText(ctx, box.x + boxSize + 8, r.y + (r.h - ctx.fontHeight) / 2.0f, label, kText);
    return changed;
}

bool radioGroup(Context& ctx, const Rect& r, const std::vector<std::string>& labels, int& selected) {
    bool changed = false;
    float x = r.x;
    float dot = r.h;
    for (size_t i = 0; i < labels.size(); i++) {
        float labelW = textWidth(ctx, labels[i]);
        Rect hit{x, r.y, dot + 8 + labelW, r.h};
        bool hover = pointIn(hit, ctx.mouseX, ctx.mouseY);
        if (hover && ctx.mouseClicked && selected != (int)i) {
            selected = (int)i;
            changed = true;
        }
        Rect dotRect{x, r.y, dot, dot};
        bool isSel = selected == (int)i;
        drawRect(ctx, dotRect, isSel ? kAccent : kPanelLite, hover ? kBorderHi : kBorder);
        if (isSel) {
            Rect inner{dotRect.x + 4, dotRect.y + 4, dotRect.w - 8, dotRect.h - 8};
            drawRect(ctx, inner, kText, kText);
        }
        drawText(ctx, x + dot + 8, r.y + (r.h - ctx.fontHeight) / 2.0f, labels[i], kText);
        x += dot + 8 + labelW + 24;
    }
    return changed;
}

// -------------------- TEXT FIELD --------------------

// Strips one UTF-8 code point (1-4 bytes) from the end of `s`.
static void popUtf8Char(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
    s.erase(i);
}

bool textField(Context& ctx, const Rect& r, int fieldId, std::string& buffer, const std::string& placeholder) {
    bool hover = pointIn(r, ctx.mouseX, ctx.mouseY);
    if (ctx.mouseClicked) {
        if (hover) ctx.focusedField = fieldId;
        else if (ctx.focusedField == fieldId) ctx.focusedField = -1;
    }
    bool focused = ctx.focusedField == fieldId;
    bool changed = false;

    if (focused) {
        if (!ctx.textInput.empty()) { buffer += ctx.textInput; changed = true; }
        if (ctx.keyBackspace && !buffer.empty()) { popUtf8Char(buffer); changed = true; }
        if (ctx.keyPaste) {
            char* clip = SDL_GetClipboardText();
            if (clip && *clip) { buffer += clip; changed = true; }
            if (clip) SDL_free(clip);
        }
        if (ctx.keyCopy) SDL_SetClipboardText(buffer.c_str());
    }

    drawRect(ctx, r, kPanel, focused ? kBorderHi : kBorder);

    SDL_Rect clip{(int)r.x, (int)r.y, (int)r.w, (int)r.h};
    SDL_SetRenderClipRect(ctx.renderer, &clip);
    float textY = r.y + (r.h - ctx.fontHeight) / 2.0f;
    if (buffer.empty() && !focused) {
        drawText(ctx, r.x + 6, textY, placeholder, kTextDim);
    } else {
        float tw = textWidth(ctx, buffer);
        // Right-align the visible window on the cursor when the text overflows the field.
        float startX = r.x + 6 - std::max(0.0f, tw - (r.w - 12));
        drawText(ctx, startX, textY, buffer, kText);
        if (focused && (SDL_GetTicks() / 500) % 2 == 0) {
            float cursorX = startX + tw + 1;
            drawRect(ctx, Rect{cursorX, r.y + 4, 2, r.h - 8}, kText, {0, 0, 0, 0});
        }
    }
    SDL_SetRenderClipRect(ctx.renderer, nullptr);

    return changed;
}

// -------------------- PATH FIELD --------------------

namespace {
std::mutex g_dialogMutex;
std::unordered_map<int, std::string> g_pendingPicks;

struct DialogUserData { int fieldId; };

void SDLCALL pathFieldDialogCallback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* ud = static_cast<DialogUserData*>(userdata);
    if (filelist && filelist[0]) {
        std::lock_guard<std::mutex> lock(g_dialogMutex);
        g_pendingPicks[ud->fieldId] = filelist[0];
    }
    delete ud;
}
} // namespace

bool pathField(Context& ctx, const Rect& r, int fieldId, std::string& buffer,
               SDL_Window* window, bool pickFolder) {
    {
        std::lock_guard<std::mutex> lock(g_dialogMutex);
        auto it = g_pendingPicks.find(fieldId);
        if (it != g_pendingPicks.end()) {
            buffer = it->second;
            g_pendingPicks.erase(it);
        }
    }

    float browseW = 90;
    Rect fieldRect{r.x, r.y, r.w - browseW - 8, r.h};
    Rect browseRect{r.x + r.w - browseW, r.y, browseW, r.h};

    bool changed = textField(ctx, fieldRect, fieldId, buffer);

    if (button(ctx, browseRect, "Browse...")) {
        auto* ud = new DialogUserData{fieldId};
        const char* defaultLoc = buffer.empty() ? nullptr : buffer.c_str();
        if (pickFolder)
            SDL_ShowOpenFolderDialog(pathFieldDialogCallback, ud, window, defaultLoc, false);
        else
            SDL_ShowOpenFileDialog(pathFieldDialogCallback, ud, window, nullptr, 0, defaultLoc, false);
    }
    return changed;
}

// -------------------- PROGRESS BAR --------------------

void progressBar(Context& ctx, const Rect& r, double fraction) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    drawRect(ctx, r, kPanel, kBorder);
    if (fraction > 0) {
        Rect fill{r.x + 2, r.y + 2, (float)((r.w - 4) * fraction), r.h - 4};
        drawRect(ctx, fill, kAccent, {0, 0, 0, 0});
    }
}

// -------------------- LOG PANEL --------------------

void logPanel(Context& ctx, const Rect& r, const std::deque<std::string>& lines, int& scrollOffset) {
    drawRect(ctx, r, kPanel, kBorder);

    int lineHeight = ctx.fontHeight + 3;
    int visibleLines = std::max(1, (int)(r.h - 8) / lineHeight);
    int total = (int)lines.size();
    int maxOffset = std::max(0, total - visibleLines);

    if (pointIn(r, ctx.mouseX, ctx.mouseY) && ctx.wheelY != 0)
        scrollOffset += (int)(ctx.wheelY * 3);
    scrollOffset = std::clamp(scrollOffset, 0, maxOffset);

    int startIndex = std::max(0, total - visibleLines - scrollOffset);
    int endIndex = std::min(total, startIndex + visibleLines);

    SDL_Rect clip{(int)r.x, (int)r.y, (int)r.w, (int)r.h};
    SDL_SetRenderClipRect(ctx.renderer, &clip);
    float y = r.y + 4;
    for (int i = startIndex; i < endIndex; i++) {
        drawText(ctx, r.x + 6, y, lines[i], kTextDim);
        y += lineHeight;
    }
    SDL_SetRenderClipRect(ctx.renderer, nullptr);
}

} // namespace ui
