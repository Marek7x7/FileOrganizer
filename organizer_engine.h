#pragma once

// Public API for the organizer's file-processing engine: everything that
// actually walks a directory and moves files, split out of main.cpp so it
// can be driven by more than one front end (the CLI in main.cpp, and the
// SDL3 GUI). The engine never writes to cout/cerr directly -- callers pass
// an EventSink and decide how (or whether) to render each event.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "organizer_core.h"

namespace fs = std::filesystem;

enum class DuplicateStrategy { RENAME, SKIP, OVERWRITE };

struct MoveRecord { std::string from, to; };

// -------------------- EVENTS --------------------

enum class EventKind { Progress, Preview, Skip, Error, Dedupe, Warning, Info };

// Meaning of `path`/`targetPath`/`message` depends on `kind`:
//   Progress: processed/total/elapsedMs are set, path/targetPath/message unused.
//   Preview:  path -> targetPath is the move that would happen.
//   Skip:     path is the file that was skipped; message is a short reason
//             (e.g. "(already exists)").
//   Error:    path is the file that failed; message is the exception text.
//   Dedupe:   path is the duplicate, targetPath is the original it duplicates;
//             message is " (skipped)" when the dedupe strategy is SKIP, empty
//             otherwise.
//   Warning/Info: message is a complete, ready-to-print line (already
//             carrying any "[WARN]"/"[SKIP]"/"[ERROR]" tag the engine wants
//             shown) with no trailing newline; a sink appends its own "\n".
//             Info is the engine's normal/success channel (stdout-ish);
//             Warning is its recoverable-problem channel (stderr-ish). Both
//             path/targetPath are unused.
struct OrganizerEvent {
    EventKind kind;
    fs::path path;
    fs::path targetPath;
    std::string message;
    size_t processed = 0;
    size_t total = 0;
    long long elapsedMs = 0;
};

using EventSink = std::function<void(const OrganizerEvent&)>;

// -------------------- OPTIONS / RESULT --------------------

struct OrganizeOptions {
    fs::path path;
    bool preview = false;
    bool recursive = false;
    bool byName = false;
    int maxDepth = 2;
    DuplicateStrategy dupStrategy = DuplicateStrategy::RENAME;
    DedupeStrategy dedupeStrategy = DedupeStrategy::NONE;
    long long minSize = -1, maxSize = -1;
    fs::file_time_type newerThan = fs::file_time_type::min();
    fs::file_time_type olderThan = fs::file_time_type::max();
};

struct OrganizeResult {
    bool ok = true;
    std::string fatalError;
    bool cancelled = false;
    bool preview = false;
    size_t total = 0, moved = 0, skipped = 0, filtered = 0, errors = 0, duplicates = 0;
    long long elapsedMs = 0;
    // Moves actually performed (empty in preview mode). The caller decides
    // whether/when to persist this via writeUndoLog -- runOrganize itself
    // never writes the log, so callers can print a summary first and match
    // the CLI's existing "summary, then undo-log line" ordering.
    std::vector<MoveRecord> undoLog;
};

// Runs a full organize pass over opts.path: collects files, optionally
// dedupes, then processes every file with a pool of worker threads,
// reporting progress/preview/skip/error/dedupe events through `sink`.
// If `cancelFlag` is non-null and observed set, stops claiming new files
// (in-flight files finish normally) and returns with result.cancelled = true.
OrganizeResult runOrganize(const OrganizeOptions& opts, const EventSink& sink,
                            std::atomic<bool>* cancelFlag = nullptr);

struct UndoResult {
    bool ok = true;
    std::string fatalError;
    int restored = 0, failed = 0;
};

// Reverses a previous run using its .organizer_log.json. Emits an Info
// event per restored/skipped move and an Error event per failure.
UndoResult performUndo(const fs::path& logPath, const EventSink& sink);

// Writes moves as a JSON undo log under rootDir. Emits an Info event with
// the log path on success. No-op (returns an empty path) if moves is empty.
fs::path writeUndoLog(const fs::path& rootDir, const std::vector<MoveRecord>& moves, const EventSink& sink);

// Resolves and reserves a target path for `file` under `targetDir`,
// synchronizing across worker threads so no two threads settle on the same
// destination. Returns nullopt if dupStrategy is SKIP and a collision was
// found. The actual fs::rename happens outside the lock this function holds
// internally.
std::optional<fs::path> resolveTargetPath(const fs::path& targetDir, const fs::path& file,
    DuplicateStrategy dupStrategy, std::mutex& dupMutex,
    std::unordered_map<std::string, int>& pathCounters,
    std::unordered_set<std::string>& reservedTargets);

// -------------------- FILE COLLECTION --------------------

// Walks a directory_iterator or recursive_directory_iterator without
// throwing: entries that fail to stat (races, dangling symlinks, permission
// errors not already filtered by skip_permission_denied) are skipped and
// reported as Warning events instead of aborting the whole scan. Checks
// cancelFlag once per entry so a Cancel during collection on a huge
// recursive tree returns quickly instead of after full enumeration.
template <typename Iterator>
void collectFiles(Iterator it, std::vector<fs::path>& files, const EventSink& sink,
                   std::atomic<bool>* cancelFlag = nullptr) {
    std::error_code ec;
    for (; it != Iterator(); it.increment(ec)) {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) return;
        if (ec) {
            sink(OrganizerEvent{EventKind::Warning, {}, {}, "[WARN] " + ec.message()});
            ec.clear();
            continue;
        }
        std::error_code statEc;
        bool isFile = fs::is_regular_file(it->path(), statEc);
        if (!statEc && isFile && !isOrganizerArtifact(it->path())) files.push_back(it->path());
    }
}
