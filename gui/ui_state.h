#pragma once

// Form/run state for the SDL3 GUI, plus the thread-safe queue used to
// ferry OrganizerEvents from the coordinator thread (running runOrganize)
// to the SDL main thread, which owns all rendering.

#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "../organizer_engine.h"

// Stable ids for text fields, used by ui::Context to track focus across
// frames (see widgets.h).
enum FieldId {
    FieldPath = 0,
    FieldMaxDepth,
    FieldMinSize,
    FieldMaxSize,
    FieldNewerThan,
    FieldOlderThan,
    FieldUndoLogPath,
};

struct UiState {
    // -------- form fields --------
    // Numeric/duration fields are kept as strings so the user can type
    // freely; they're parsed with organizer_core.h's parseSize/parseDuration/
    // parseMaxDepth right before a run starts, same as the CLI parses argv.
    std::string pathBuf;
    bool recursive = false;
    bool byName = false;
    std::string maxDepthBuf = "2";
    int dupStrategyIndex = 0;   // rename / skip / overwrite
    bool preview = false;
    int dedupeIndex = 0;        // off / skip / report
    std::string minSizeBuf, maxSizeBuf, newerThanBuf, olderThanBuf;

    std::string undoLogPathBuf;

    // -------- run state --------
    bool running = false;
    std::atomic<bool> cancelFlag{false};
    std::future<OrganizeResult> future;
    bool hasResult = false;
    OrganizeResult lastResult;
    DedupeStrategy lastDedupeStrategy = DedupeStrategy::NONE; // needed to word the summary's duplicates line

    size_t progressDone = 0, progressTotal = 0;

    // -------- log --------
    std::deque<std::string> logLines;
    int logScroll = 0;
    static constexpr size_t kMaxLogLines = 2000;

    void appendLog(const std::string& line) {
        // A message may itself contain embedded newlines (mirroring how the
        // CLI prints them); split so the log panel's one-entry-per-line
        // assumption holds.
        size_t start = 0;
        while (true) {
            size_t nl = line.find('\n', start);
            std::string part = line.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            logLines.push_back(part);
            if (logLines.size() > kMaxLogLines) logLines.pop_front();
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
};

// Pushed to by the coordinator thread's EventSink, drained once per frame
// by the SDL main thread. Its own mutex is separate from the engine's
// internal logMutex -- this just protects the hand-off buffer.
struct EventQueue {
    std::mutex mutex;
    std::vector<OrganizerEvent> events;

    void push(const OrganizerEvent& e) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }

    std::vector<OrganizerEvent> drain(size_t maxCount) {
        std::lock_guard<std::mutex> lock(mutex);
        if (events.size() <= maxCount) return std::move(events);
        std::vector<OrganizerEvent> out(events.begin(), events.begin() + maxCount);
        events.erase(events.begin(), events.begin() + maxCount);
        return out;
    }
};
