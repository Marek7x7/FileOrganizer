#include "organizer_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

// -------------------- DUPLICATE TARGET RESOLUTION --------------------

std::optional<fs::path> resolveTargetPath(const fs::path& targetDir, const fs::path& file,
    DuplicateStrategy dupStrategy, std::mutex& dupMutex,
    std::unordered_map<std::string, int>& pathCounters,
    std::unordered_set<std::string>& reservedTargets) {
    fs::path targetPath = targetDir / file.filename();
    bool existsOnDisk = fs::exists(targetPath);

    std::lock_guard<std::mutex> lock(dupMutex);

    bool collision = existsOnDisk || reservedTargets.count(targetPath.string());
    if (collision) {
        if (dupStrategy == DuplicateStrategy::SKIP) return std::nullopt;
        if (dupStrategy == DuplicateStrategy::RENAME)
            targetPath = getUniquePathLocked(targetPath, pathCounters, reservedTargets);
        // OVERWRITE: keep targetPath as-is; fs::rename will replace it
    }

    reservedTargets.insert(targetPath.string());
    return targetPath;
}

// -------------------- UNDO LOG --------------------

fs::path writeUndoLog(const fs::path& rootDir, const std::vector<MoveRecord>& moves, const EventSink& sink) {
    if (moves.empty()) return {};
    fs::path logPath = rootDir / ".organizer_log.json";

    std::time_t t = std::time(nullptr);
    std::tm* ti = std::localtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ti);

    std::ofstream log(logPath);
    if (!log) {
        std::ostringstream oss;
        oss << "[WARN] Could not write undo log to " << logPath;
        sink(OrganizerEvent{EventKind::Warning, {}, {}, oss.str()});
        return {};
    }

    auto escape = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;      break;
            }
        }
        return out;
    };

    log << "{\n  \"timestamp\": \"" << buf << "\",\n  \"moves\": [\n";
    for (size_t i = 0; i < moves.size(); ++i) {
        log << "    {\"from\": \"" << escape(moves[i].from)
            << "\", \"to\": \""   << escape(moves[i].to) << "\"}";
        if (i + 1 < moves.size()) log << ",";
        log << "\n";
    }
    log << "  ]\n}\n";

    std::ostringstream oss;
    oss << "Undo log: " << logPath;
    sink(OrganizerEvent{EventKind::Info, {}, {}, oss.str()});
    return logPath;
}

UndoResult performUndo(const fs::path& logPath, const EventSink& sink) {
    UndoResult result;
    std::ifstream log(logPath);
    if (!log) {
        std::ostringstream oss;
        oss << "[FATAL] Cannot open log: " << logPath;
        sink(OrganizerEvent{EventKind::Warning, {}, {}, oss.str()});
        std::ostringstream fatal;
        fatal << "Cannot open log: " << logPath;
        result.ok = false;
        result.fatalError = fatal.str();
        return result;
    }

    // Reads a JSON string value starting right after its opening quote, honoring
    // backslash escapes so a `\"` embedded in a path can't be mistaken for the
    // string's closing quote.
    auto extractJsonString = [](const std::string& line, size_t start) -> std::string {
        std::string out;
        for (size_t i = start; i < line.size() && line[i] != '"'; ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                char next = line[++i];
                switch (next) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default:  out += next; break; // \\ and \" collapse to the literal char
                }
            } else {
                out += line[i];
            }
        }
        return out;
    };

    auto extract = [&extractJsonString](const std::string& line, const std::string& key) -> std::string {
        size_t pos = line.find(key);
        if (pos == std::string::npos) return "";
        return extractJsonString(line, pos + key.size());
    };

    std::string line;
    while (std::getline(log, line)) {
        std::string from = extract(line, "\"from\": \"");
        std::string to   = extract(line, "\"to\": \"");
        if (from.empty() || to.empty()) continue;
        try {
            if (fs::exists(to)) {
                fs::rename(to, from);
                sink(OrganizerEvent{EventKind::Info, {}, {}, "Restored: " + to + "\n        -> " + from});
                result.restored++;
            } else {
                sink(OrganizerEvent{EventKind::Warning, {}, {}, "[SKIP] No longer exists: " + to});
            }
        } catch (const std::exception& e) {
            sink(OrganizerEvent{EventKind::Warning, {}, {}, std::string("[ERROR] ") + e.what()});
            result.failed++;
        }
    }

    std::ostringstream oss;
    oss << "\nUndo complete: " << result.restored << " restored, " << result.failed << " failed";
    sink(OrganizerEvent{EventKind::Info, {}, {}, oss.str()});
    return result;
}

// -------------------- WORKER (multithreaded file processing) --------------------

namespace {

struct WorkerContext {
    const std::vector<fs::path>& files;
    const std::unordered_map<std::string, std::string>& categories;
    bool preview, recursive, byName;
    int maxDepth;
    DuplicateStrategy dupStrategy;
    long long minSize, maxSize;
    fs::file_time_type newerThan, olderThan;
    fs::path rootPath;
    size_t total, progressStep;
    std::chrono::steady_clock::time_point startTime;

    std::atomic<size_t>& processed;
    std::atomic<size_t>& moved;
    std::atomic<size_t>& skipped;
    std::atomic<size_t>& filtered;
    std::atomic<size_t>& errors;

    std::mutex& dirCacheMutex;
    std::unordered_set<std::string>& createdDirs;
    std::mutex& dupMutex;
    std::unordered_map<std::string, int>& pathCounters;
    std::unordered_set<std::string>& reservedTargets;
    std::mutex& logMutex;

    const EventSink& sink;
    std::atomic<bool>* cancelFlag;
};

void maybePrintProgress(WorkerContext& ctx, size_t p) {
    if (p % ctx.progressStep == 0 || p == ctx.total) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ctx.startTime).count();
        std::lock_guard<std::mutex> lock(ctx.logMutex);
        ctx.sink(OrganizerEvent{EventKind::Progress, {}, {}, "", p, ctx.total, ms});
    }
}

// Does the actual work for processFile. Filesystem calls here (file_size,
// last_write_time, create_directories, exists) can throw if a file vanishes
// or becomes unreadable mid-run (races with other processes/AV scanners);
// the caller wraps this in try/catch so one bad file can't take down a
// worker thread via an uncaught exception.
void processFileImpl(const fs::path& file, WorkerContext& ctx, std::vector<MoveRecord>& localUndoLog, size_t p) {
    // ---- SIZE / DATE FILTERS ----
    bool skip = false;

    if (ctx.minSize >= 0 || ctx.maxSize >= 0) {
        auto sz = (long long)fs::file_size(file);
        if (ctx.minSize >= 0 && sz < ctx.minSize) skip = true;
        if (ctx.maxSize >= 0 && sz > ctx.maxSize) skip = true;
    }
    if (!skip && (ctx.newerThan != fs::file_time_type::min() ||
        ctx.olderThan != fs::file_time_type::max())) {
        auto mtime = fs::last_write_time(file);
        if (ctx.newerThan != fs::file_time_type::min() && mtime < ctx.newerThan) skip = true;
        if (ctx.olderThan != fs::file_time_type::max() && mtime > ctx.olderThan) skip = true;
    }
    if (skip) {
        ctx.filtered.fetch_add(1, std::memory_order_relaxed);
        maybePrintProgress(ctx, p);
        return;
    }

    // ---- TARGET PATH ----
    fs::path targetDir;

    if (ctx.byName) {
        std::vector<std::string> parts = getNameParts(file);
        if ((int)parts.size() > ctx.maxDepth) parts.resize(ctx.maxDepth);
        targetDir = ctx.recursive ? file.parent_path() : ctx.rootPath;
        if (parts.empty()) targetDir /= "Other";
        else for (const std::string& part : parts) targetDir /= part;
    } else {
        std::string ext = file.extension().string();
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        auto it = ctx.categories.find(ext);
        std::string category = (it != ctx.categories.end()) ? it->second : "Other";
        targetDir = (ctx.recursive ? file.parent_path() : ctx.rootPath) / category;
    }

    // ---- CREATE DIR (cached) ----
    std::string dirKey = targetDir.string();
    bool needsCreate;
    {
        std::lock_guard<std::mutex> lock(ctx.dirCacheMutex);
        needsCreate = ctx.createdDirs.insert(dirKey).second;
    }
    if (needsCreate && !fs::exists(targetDir)) fs::create_directories(targetDir);

    // ---- DUPLICATE HANDLING ----
    auto resolved = resolveTargetPath(targetDir, file, ctx.dupStrategy, ctx.dupMutex, ctx.pathCounters, ctx.reservedTargets);

    if (!resolved) {
        {
            std::lock_guard<std::mutex> lock(ctx.logMutex);
            ctx.sink(OrganizerEvent{EventKind::Skip, file, {}, "(already exists)"});
        }
        ctx.skipped.fetch_add(1, std::memory_order_relaxed);
        maybePrintProgress(ctx, p);
        return;
    }
    fs::path targetPath = *resolved;

    // ---- MOVE / PREVIEW ----
    if (ctx.preview) {
        {
            std::lock_guard<std::mutex> lock(ctx.logMutex);
            ctx.sink(OrganizerEvent{EventKind::Preview, file, targetPath, ""});
        }
        ctx.skipped.fetch_add(1, std::memory_order_relaxed);
    } else {
        try {
            fs::rename(file, targetPath);
            localUndoLog.push_back({file.string(), targetPath.string()});
            ctx.moved.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(ctx.logMutex);
            ctx.sink(OrganizerEvent{EventKind::Error, file, {}, e.what()});
            ctx.errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    maybePrintProgress(ctx, p);
}

void processFile(const fs::path& file, WorkerContext& ctx, std::vector<MoveRecord>& localUndoLog) {
    size_t p = ctx.processed.fetch_add(1, std::memory_order_relaxed) + 1;
    try {
        processFileImpl(file, ctx, localUndoLog, p);
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(ctx.logMutex);
            ctx.sink(OrganizerEvent{EventKind::Error, file, {}, e.what()});
        }
        ctx.errors.fetch_add(1, std::memory_order_relaxed);
        maybePrintProgress(ctx, p);
    }
}

void worker(WorkerContext& ctx, std::atomic<size_t>& nextIndex, std::vector<MoveRecord>& localUndoLog) {
    size_t idx;
    while (!(ctx.cancelFlag && ctx.cancelFlag->load(std::memory_order_relaxed)) &&
           (idx = nextIndex.fetch_add(1, std::memory_order_relaxed)) < ctx.total) {
        processFile(ctx.files[idx], ctx, localUndoLog);
    }
}

} // namespace

// -------------------- ENGINE ENTRY POINT --------------------

OrganizeResult runOrganize(const OrganizeOptions& opts, const EventSink& sink, std::atomic<bool>* cancelFlag) {
    OrganizeResult result;
    result.preview = opts.preview;

    auto categories = loadCategories(opts.path);

    // -------- FILE COLLECTION --------
    std::vector<fs::path> files;
    try {
        auto dirOpts = fs::directory_options::skip_permission_denied;
        std::error_code ec;
        if (opts.recursive) {
            fs::recursive_directory_iterator it(opts.path, dirOpts, ec);
            if (ec) throw fs::filesystem_error("Cannot open directory", opts.path, ec);
            collectFiles(std::move(it), files, sink, cancelFlag);
        } else {
            fs::directory_iterator it(opts.path, dirOpts, ec);
            if (ec) throw fs::filesystem_error("Cannot open directory", opts.path, ec);
            collectFiles(std::move(it), files, sink, cancelFlag);
        }
    } catch (const std::exception& e) {
        result.ok = false;
        result.fatalError = e.what();
        return result;
    }

    if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
    }

    if (files.empty()) {
        sink(OrganizerEvent{EventKind::Info, {}, {}, "No files found."});
        return result;
    }

    // -------- DEDUPE --------
    size_t dedupeCount = 0;
    if (opts.dedupeStrategy != DedupeStrategy::NONE) {
        auto dedupe = findDuplicates(files, opts.dedupeStrategy);
        dedupeCount = dedupe.duplicates.size();
        bool skipping = (opts.dedupeStrategy == DedupeStrategy::SKIP);
        for (auto& [dup, original] : dedupe.duplicates) {
            sink(OrganizerEvent{EventKind::Dedupe, dup, original, skipping ? " (skipped)" : ""});
        }
        if (skipping) files = std::move(dedupe.keep);
    }

    if (files.empty()) {
        sink(OrganizerEvent{EventKind::Info, {}, {}, "No files left to process after dedupe."});
        result.duplicates = dedupeCount;
        return result;
    }

    size_t total = files.size();
    {
        std::ostringstream oss;
        oss << "Found " << total << " files. Processing...\n";
        sink(OrganizerEvent{EventKind::Info, {}, {}, oss.str()});
    }

    // -------- WORKER POOL --------
    size_t progressStep = std::max((size_t)1, total / 20);
    auto startTime = std::chrono::steady_clock::now();

    std::atomic<size_t> processed{0}, moved{0}, skipped{0}, filtered{0}, errors{0};
    std::mutex dirCacheMutex, dupMutex, logMutex;
    std::unordered_set<std::string> createdDirs, reservedTargets;
    std::unordered_map<std::string, int> pathCounters;

    WorkerContext ctx{
        files, categories, opts.preview, opts.recursive, opts.byName, opts.maxDepth, opts.dupStrategy,
        opts.minSize, opts.maxSize, opts.newerThan, opts.olderThan, opts.path, total, progressStep, startTime,
        processed, moved, skipped, filtered, errors,
        dirCacheMutex, createdDirs, dupMutex, pathCounters, reservedTargets, logMutex,
        sink, cancelFlag
    };

    unsigned int hw = std::thread::hardware_concurrency();
    unsigned int numThreads = hw == 0 ? 4 : hw;
    numThreads = (unsigned int)std::min<size_t>(numThreads, total);
    numThreads = std::max(numThreads, 1u);
    if (total < 64) numThreads = 1; // not worth spinning up threads for tiny runs

    std::atomic<size_t> nextIndex{0};
    std::vector<std::vector<MoveRecord>> perThreadLogs(numThreads);
    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < numThreads; t++)
        workers.emplace_back(worker, std::ref(ctx), std::ref(nextIndex), std::ref(perThreadLogs[t]));
    for (auto& t : workers) t.join();

    std::vector<MoveRecord> undoLog;
    for (auto& log : perThreadLogs)
        undoLog.insert(undoLog.end(), std::make_move_iterator(log.begin()), std::make_move_iterator(log.end()));

    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    result.total = total;
    result.moved = moved.load();
    result.skipped = skipped.load();
    result.filtered = filtered.load();
    result.errors = errors.load();
    result.duplicates = dedupeCount;
    result.elapsedMs = totalMs;
    result.cancelled = cancelFlag && cancelFlag->load(std::memory_order_relaxed);
    if (!opts.preview) result.undoLog = std::move(undoLog);

    return result;
}
