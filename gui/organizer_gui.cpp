// SDL3 GUI front end for FileOrganizer. Drives the same organizer_engine
// the CLI (main.cpp) uses -- no subprocess, no duplicated worker logic.
//
// Threading: each run is kicked off via std::async on a coordinator
// thread; its EventSink pushes onto a small thread-safe EventQueue that
// this file's SDL main-thread loop drains once per frame. SDL rendering
// state is only ever touched from the main thread.

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <sstream>
#include <string>

#include "ui_state.h"
#include "widgets.h"

using ui::Rect;
namespace fs = std::filesystem;

static const int kWinW = 860;
static const int kWinH = 820;

// -------------------- EVENT -> LOG/PROGRESS --------------------

static void applyEvent(UiState& state, const OrganizerEvent& e) {
    switch (e.kind) {
        case EventKind::Progress:
            state.progressDone = e.processed;
            state.progressTotal = e.total;
            break;
        case EventKind::Preview: {
            std::ostringstream oss;
            oss << "[PREVIEW] " << e.path.filename().string() << "\n          -> " << e.targetPath;
            state.appendLog(oss.str());
            break;
        }
        case EventKind::Skip:
            state.appendLog("[SKIP] " + e.path.filename().string() + " " + e.message);
            break;
        case EventKind::Error:
            state.appendLog("[ERROR] " + e.path.filename().string() + ": " + e.message);
            break;
        case EventKind::Dedupe: {
            std::ostringstream oss;
            oss << "[DEDUPE] " << e.path.filename().string() << " duplicates "
                << e.targetPath.filename().string() << e.message;
            state.appendLog(oss.str());
            break;
        }
        case EventKind::Warning:
        case EventKind::Info:
            state.appendLog(e.message);
            break;
    }
}

static void appendSummary(UiState& state) {
    OrganizeResult& r = state.lastResult;
    if (!r.ok) {
        state.appendLog("[FATAL] " + r.fatalError);
        return;
    }
    if (r.cancelled) state.appendLog("Cancelled.");
    if (r.total == 0) return; // "No files found."/"No files left..." already logged via events

    std::ostringstream oss;
    oss << "--- Done in " << r.elapsedMs << "ms ---";
    state.appendLog(oss.str());
    if (r.preview) state.appendLog("  Would move: " + std::to_string(r.total - r.filtered - r.skipped));
    else           state.appendLog("  Moved:      " + std::to_string(r.moved));
    if (r.skipped)    state.appendLog("  Skipped:    " + std::to_string(r.skipped));
    if (r.filtered)   state.appendLog("  Filtered:   " + std::to_string(r.filtered) + " (size/date criteria)");
    if (r.duplicates) state.appendLog("  Duplicates: " + std::to_string(r.duplicates) +
        (state.lastDedupeStrategy == DedupeStrategy::SKIP ? " (skipped)" : " (reported)"));
    if (r.errors)     state.appendLog("  Errors:     " + std::to_string(r.errors));

    if (!r.preview && !r.undoLog.empty()) {
        EventSink directSink = [&state](const OrganizerEvent& e) { applyEvent(state, e); };
        writeUndoLog(fs::path(state.pathBuf), r.undoLog, directSink);
    }
}

// -------------------- OPTIONS FROM FORM STATE --------------------

static bool buildOptions(UiState& state, OrganizeOptions& opts) {
    if (state.pathBuf.empty()) {
        state.appendLog("[FATAL] No target directory selected.");
        return false;
    }
    fs::path path = state.pathBuf;
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) {
        std::ostringstream oss;
        oss << "[FATAL] Invalid directory: " << path;
        state.appendLog(oss.str());
        return false;
    }

    opts.path = path;
    opts.recursive = state.recursive;
    opts.byName = state.byName;
    opts.preview = state.preview;
    opts.maxDepth = parseMaxDepth(state.maxDepthBuf, 2);
    opts.dupStrategy = state.dupStrategyIndex == 1 ? DuplicateStrategy::SKIP
                      : state.dupStrategyIndex == 2 ? DuplicateStrategy::OVERWRITE
                      : DuplicateStrategy::RENAME;
    opts.dedupeStrategy = state.dedupeIndex == 1 ? DedupeStrategy::SKIP
                         : state.dedupeIndex == 2 ? DedupeStrategy::REPORT
                         : DedupeStrategy::NONE;
    opts.minSize = state.minSizeBuf.empty() ? -1 : parseSize(state.minSizeBuf);
    opts.maxSize = state.maxSizeBuf.empty() ? -1 : parseSize(state.maxSizeBuf);
    opts.newerThan = state.newerThanBuf.empty() ? fs::file_time_type::min() : parseDuration(state.newerThanBuf);
    opts.olderThan = state.olderThanBuf.empty() ? fs::file_time_type::max() : parseDuration(state.olderThanBuf);
    return true;
}

// -------------------- LAYOUT --------------------

static void layoutAndDraw(ui::Context& ctx, UiState& state, SDL_Window* window, const EventSink& guiSink) {
    using namespace ui;
    const float pad = 16;
    const float rowH = 28;
    const float gap = 10;
    const float fullW = kWinW - pad * 2;
    float y = pad;

    drawText(ctx, pad, y, "Target directory", kTextDim);
    y += 18;
    pathField(ctx, Rect{pad, y, fullW, rowH}, FieldPath, state.pathBuf, window, true);
    y += rowH + gap;

    float cbW = fullW / 3.0f;
    checkbox(ctx, Rect{pad, y, cbW, rowH}, "Recursive", state.recursive);
    checkbox(ctx, Rect{pad + cbW, y, cbW, rowH}, "By name", state.byName);
    checkbox(ctx, Rect{pad + cbW * 2, y, cbW, rowH}, "Preview (dry run)", state.preview);
    y += rowH + gap;

    drawText(ctx, pad, y + 6, "Max depth (--by-name):", kTextDim);
    textField(ctx, Rect{pad + 190, y, 60, rowH}, FieldMaxDepth, state.maxDepthBuf);
    y += rowH + gap;

    drawText(ctx, pad, y, "On duplicate", kTextDim);
    y += 18;
    radioGroup(ctx, Rect{pad, y, fullW, rowH}, {"Rename", "Skip", "Overwrite"}, state.dupStrategyIndex);
    y += rowH + gap;

    drawText(ctx, pad, y, "Content dedupe", kTextDim);
    y += 18;
    radioGroup(ctx, Rect{pad, y, fullW, rowH}, {"Off", "Skip", "Report"}, state.dedupeIndex);
    y += rowH + gap;

    float halfW = (fullW - gap) / 2.0f;
    drawText(ctx, pad, y + 6, "Min size:", kTextDim);
    textField(ctx, Rect{pad + 80, y, halfW - 80, rowH}, FieldMinSize, state.minSizeBuf, "e.g. 1MB");
    drawText(ctx, pad + halfW + gap, y + 6, "Max size:", kTextDim);
    textField(ctx, Rect{pad + halfW + gap + 80, y, halfW - 80, rowH}, FieldMaxSize, state.maxSizeBuf, "e.g. 1GB");
    y += rowH + gap;

    drawText(ctx, pad, y + 6, "Newer than:", kTextDim);
    textField(ctx, Rect{pad + 90, y, halfW - 90, rowH}, FieldNewerThan, state.newerThanBuf, "e.g. 7d");
    drawText(ctx, pad + halfW + gap, y + 6, "Older than:", kTextDim);
    textField(ctx, Rect{pad + halfW + gap + 90, y, halfW - 90, rowH}, FieldOlderThan, state.olderThanBuf, "e.g. 30d");
    y += rowH + gap;

    float btnW = 110;
    bool runClicked = button(ctx, Rect{pad, y, btnW, rowH}, state.running ? "Running..." : "Run", !state.running);
    bool cancelClicked = button(ctx, Rect{pad + btnW + 8, y, btnW, rowH}, "Cancel", state.running);
    progressBar(ctx, Rect{pad + (btnW + 8) * 2, y, fullW - (btnW + 8) * 2, rowH},
                state.progressTotal ? (double)state.progressDone / (double)state.progressTotal : 0.0);
    y += rowH + gap;

    if (runClicked && !state.running) {
        OrganizeOptions opts;
        if (buildOptions(state, opts)) {
            state.running = true;
            state.hasResult = false;
            state.cancelFlag.store(false);
            state.progressDone = 0;
            state.progressTotal = 0;
            state.lastDedupeStrategy = opts.dedupeStrategy;
            state.future = std::async(std::launch::async, runOrganize, opts, guiSink, &state.cancelFlag);
        }
    }
    if (cancelClicked && state.running) state.cancelFlag.store(true);

    float undoRowY = kWinH - pad - rowH;
    float logH = undoRowY - gap - y;
    logPanel(ctx, Rect{pad, y, fullW, logH}, state.logLines, state.logScroll);

    drawText(ctx, pad, undoRowY + 6, "Undo log:", kTextDim);
    pathField(ctx, Rect{pad + 90, undoRowY, fullW - 90 - 100 - 8, rowH}, FieldUndoLogPath, state.undoLogPathBuf, window, false);
    bool restoreClicked = button(ctx, Rect{pad + fullW - 100, undoRowY, 100, rowH}, "Restore", !state.running);
    if (restoreClicked && !state.running && !state.undoLogPathBuf.empty()) {
        EventSink directSink = [&state](const OrganizerEvent& e) { applyEvent(state, e); };
        performUndo(fs::path(state.undoLogPathBuf), directSink);
    }
}

// -------------------- FONT LOCATION --------------------

static std::string findFontPath(const char* argv0) {
    std::error_code ec;
    fs::path exe = fs::absolute(fs::path(argv0), ec);
    if (ec) return "";
    fs::path exeDir = exe.parent_path();
    for (fs::path candidate : {exeDir / "assets" / "DejaVuSans.ttf", exeDir / ".." / "assets" / "DejaVuSans.ttf"}) {
        if (fs::exists(candidate)) return candidate.string();
    }
    return "";
}

// -------------------- MAIN --------------------

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) {
        SDL_Log("TTF_Init failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    std::string fontPath = findFontPath(argv[0]);
    if (fontPath.empty()) {
        SDL_Log("Could not locate assets/DejaVuSans.ttf next to the executable");
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("File Organizer", kWinW, kWinH, 0, &window, &renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    TTF_Font* font = TTF_OpenFont(fontPath.c_str(), 15.0f);
    if (!font) {
        SDL_Log("TTF_OpenFont failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_StartTextInput(window);

    ui::Context ctx;
    ctx.renderer = renderer;
    ctx.font = font;
    ctx.fontHeight = 15;

    UiState state;
    EventQueue queue;
    EventSink guiSink = [&queue](const OrganizerEvent& e) { queue.push(e); };

    bool wantQuit = false;
    while (!wantQuit) {
        // Must run before polling: it clears the previous frame's transient
        // input (mouseClicked, textInput, ...) so this frame's events below
        // start from a clean slate instead of being wiped right after they're
        // recorded.
        ctx.beginFrame();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    wantQuit = true;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    ctx.onMouseMotion(ev.motion.x, ev.motion.y);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT)
                        ctx.onMouseButton(ev.button.down, ev.button.x, ev.button.y);
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    ctx.onMouseWheel(ev.wheel.y);
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    ctx.onTextInput(ev.text.text);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    ctx.onKeyDown(ev.key.key, ev.key.mod);
                    break;
                default:
                    break;
            }
        }

        if (wantQuit) {
            if (state.running) {
                state.cancelFlag.store(true);
                state.future.wait();
            }
            break;
        }

        if (state.running) {
            for (auto& e : queue.drain(500)) applyEvent(state, e);
            if (state.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                state.lastResult = state.future.get();
                state.hasResult = true;
                state.running = false;
                appendSummary(state);
            }
        }

        SDL_SetRenderDrawColor(renderer, ui::kBg.r, ui::kBg.g, ui::kBg.b, ui::kBg.a);
        SDL_RenderClear(renderer);

        layoutAndDraw(ctx, state, window, guiSink);

        ctx.endFrame();
    }

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
