#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <iomanip>
#include <chrono>
#include <fstream>
#include <ctime>
#include <thread>
#include <mutex>
#include <atomic>
#include <optional>
#include <system_error>
#include <stdexcept>

#include "organizer_core.h"

using namespace std;

// -------------------- ENUMS --------------------

enum class DuplicateStrategy { RENAME, SKIP, OVERWRITE };

// -------------------- FILE UTILS --------------------

// Resolves and reserves a target path for `file` under `targetDir`, synchronizing
// across worker threads so no two threads settle on the same destination.
// Returns nullopt if the duplicate strategy is SKIP and a collision was found.
// The actual fs::rename happens outside the lock this function holds internally.
optional<fs::path> resolveTargetPath(const fs::path& targetDir, const fs::path& file,
    DuplicateStrategy dupStrategy, mutex& dupMutex,
    unordered_map<string, int>& pathCounters,
    unordered_set<string>& reservedTargets) {
    fs::path targetPath = targetDir / file.filename();
    bool existsOnDisk = fs::exists(targetPath);

    lock_guard<mutex> lock(dupMutex);

    bool collision = existsOnDisk || reservedTargets.count(targetPath.string());
    if (collision) {
        if (dupStrategy == DuplicateStrategy::SKIP) return nullopt;
        if (dupStrategy == DuplicateStrategy::RENAME)
            targetPath = getUniquePathLocked(targetPath, pathCounters, reservedTargets);
        // OVERWRITE: keep targetPath as-is; fs::rename will replace it
    }

    reservedTargets.insert(targetPath.string());
    return targetPath;
}

// -------------------- UNDO LOG --------------------

struct MoveRecord { string from, to; };

void writeUndoLog(const fs::path& rootDir, const vector<MoveRecord>& moves) {
    if (moves.empty()) return;
    fs::path logPath = rootDir / ".organizer_log.json";

    time_t t = time(nullptr);
    tm* ti = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ti);

    ofstream log(logPath);
    if (!log) { cerr << "[WARN] Could not write undo log to " << logPath << "\n"; return; }

    auto escape = [](const string& s) {
        string out;
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
    cout << "Undo log: " << logPath << "\n";
}

void performUndo(const fs::path& logPath) {
    ifstream log(logPath);
    if (!log) { cerr << "[FATAL] Cannot open log: " << logPath << "\n"; return; }

    // Reads a JSON string value starting right after its opening quote, honoring
    // backslash escapes so a `\"` embedded in a path can't be mistaken for the
    // string's closing quote (the previous implementation used a plain
    // line.find('"') that broke on quote characters inside a path).
    auto extractJsonString = [](const string& line, size_t start) -> string {
        string out;
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

    auto extract = [&extractJsonString](const string& line, const string& key) -> string {
        size_t pos = line.find(key);
        if (pos == string::npos) return "";
        return extractJsonString(line, pos + key.size());
    };

    string line;
    int restored = 0, failed = 0;
    while (getline(log, line)) {
        string from = extract(line, "\"from\": \"");
        string to   = extract(line, "\"to\": \"");
        if (from.empty() || to.empty()) continue;
        try {
            if (fs::exists(to)) {
                fs::rename(to, from);
                cout << "Restored: " << to << "\n        -> " << from << "\n";
                restored++;
            } else {
                cerr << "[SKIP] No longer exists: " << to << "\n";
            }
        } catch (const exception& e) {
            cerr << "[ERROR] " << e.what() << "\n";
            failed++;
        }
    }
    cout << "\nUndo complete: " << restored << " restored, " << failed << " failed\n";
}

// -------------------- WORKER (multithreaded file processing) --------------------

struct WorkerContext {
    const vector<fs::path>& files;
    const unordered_map<string, string>& categories;
    bool preview, recursive, byName;
    int maxDepth;
    DuplicateStrategy dupStrategy;
    long long minSize, maxSize;
    fs::file_time_type newerThan, olderThan;
    fs::path rootPath;
    size_t total, progressStep;
    chrono::steady_clock::time_point startTime;

    atomic<size_t>& processed;
    atomic<size_t>& moved;
    atomic<size_t>& skipped;
    atomic<size_t>& filtered;
    atomic<size_t>& errors;

    mutex& dirCacheMutex;
    unordered_set<string>& createdDirs;
    mutex& dupMutex;
    unordered_map<string, int>& pathCounters;
    unordered_set<string>& reservedTargets;
    mutex& logMutex;
};

void maybePrintProgress(WorkerContext& ctx, size_t p) {
    if (p % ctx.progressStep == 0 || p == ctx.total) {
        double pct = (double)p / ctx.total * 100.0;
        auto ms = chrono::duration_cast<chrono::milliseconds>(
            chrono::steady_clock::now() - ctx.startTime).count();
        lock_guard<mutex> lock(ctx.logMutex);
        // \r + clear-to-end-of-line redraws this line in place instead of scrolling;
        // no trailing '\n' so the next progress update overwrites it again.
        cout << "\r\033[K" << fixed << setprecision(1)
             << "  " << pct << "% (" << p << "/" << ctx.total
             << ") [" << ms << "ms]" << flush;
    }
}

// Does the actual work for processFile. Filesystem calls here (file_size,
// last_write_time, create_directories, exists) can throw if a file vanishes
// or becomes unreadable mid-run (races with other processes/AV scanners);
// the caller wraps this in try/catch so one bad file can't take down a
// worker thread via an uncaught exception.
void processFileImpl(const fs::path& file, WorkerContext& ctx, vector<MoveRecord>& localUndoLog, size_t p) {
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
        ctx.filtered.fetch_add(1, memory_order_relaxed);
        maybePrintProgress(ctx, p);
        return;
    }

    // ---- TARGET PATH ----
    fs::path targetDir;

    if (ctx.byName) {
        vector<string> parts = getNameParts(file);
        if ((int)parts.size() > ctx.maxDepth) parts.resize(ctx.maxDepth);
        targetDir = ctx.recursive ? file.parent_path() : ctx.rootPath;
        if (parts.empty()) targetDir /= "Other";
        else for (const string& part : parts) targetDir /= part;
    } else {
        string ext = file.extension().string();
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        auto it = ctx.categories.find(ext);
        string category = (it != ctx.categories.end()) ? it->second : "Other";
        targetDir = (ctx.recursive ? file.parent_path() : ctx.rootPath) / category;
    }

    // ---- CREATE DIR (cached) ----
    string dirKey = targetDir.string();
    bool needsCreate;
    {
        lock_guard<mutex> lock(ctx.dirCacheMutex);
        needsCreate = ctx.createdDirs.insert(dirKey).second;
    }
    if (needsCreate && !fs::exists(targetDir)) fs::create_directories(targetDir);

    // ---- DUPLICATE HANDLING ----
    auto resolved = resolveTargetPath(targetDir, file, ctx.dupStrategy, ctx.dupMutex, ctx.pathCounters, ctx.reservedTargets);

    if (!resolved) {
        {
            lock_guard<mutex> lock(ctx.logMutex);
            cout << "\r\033[K[SKIP] " << file.filename().string() << " (already exists)\n";
        }
        ctx.skipped.fetch_add(1, memory_order_relaxed);
        maybePrintProgress(ctx, p);
        return;
    }
    fs::path targetPath = *resolved;

    // ---- MOVE / PREVIEW ----
    if (ctx.preview) {
        {
            lock_guard<mutex> lock(ctx.logMutex);
            cout << "\r\033[K[PREVIEW] " << file.filename().string()
                 << "\n          -> " << targetPath << "\n";
        }
        ctx.skipped.fetch_add(1, memory_order_relaxed);
    } else {
        try {
            fs::rename(file, targetPath);
            localUndoLog.push_back({file.string(), targetPath.string()});
            ctx.moved.fetch_add(1, memory_order_relaxed);
        } catch (const exception& e) {
            lock_guard<mutex> lock(ctx.logMutex);
            cerr << "\r\033[K[ERROR] " << file.filename().string() << ": " << e.what() << "\n";
            ctx.errors.fetch_add(1, memory_order_relaxed);
        }
    }

    maybePrintProgress(ctx, p);
}

void processFile(const fs::path& file, WorkerContext& ctx, vector<MoveRecord>& localUndoLog) {
    size_t p = ctx.processed.fetch_add(1, memory_order_relaxed) + 1;
    try {
        processFileImpl(file, ctx, localUndoLog, p);
    } catch (const exception& e) {
        {
            lock_guard<mutex> lock(ctx.logMutex);
            cerr << "\r\033[K[ERROR] " << file.filename().string() << ": " << e.what() << "\n";
        }
        ctx.errors.fetch_add(1, memory_order_relaxed);
        maybePrintProgress(ctx, p);
    }
}

void worker(WorkerContext& ctx, atomic<size_t>& nextIndex, vector<MoveRecord>& localUndoLog) {
    size_t idx;
    while ((idx = nextIndex.fetch_add(1, memory_order_relaxed)) < ctx.total) {
        processFile(ctx.files[idx], ctx, localUndoLog);
    }
}

// -------------------- FILE COLLECTION --------------------

// Walks a directory_iterator or recursive_directory_iterator without throwing:
// entries that fail to stat (races, dangling symlinks, permission errors not
// already filtered by skip_permission_denied) are skipped and reported instead
// of aborting the whole scan.
template <typename Iterator>
void collectFiles(Iterator it, vector<fs::path>& files) {
    error_code ec;
    for (; it != Iterator(); it.increment(ec)) {
        if (ec) {
            cerr << "[WARN] " << ec.message() << "\n";
            ec.clear();
            continue;
        }
        error_code statEc;
        bool isFile = fs::is_regular_file(it->path(), statEc);
        if (!statEc && isFile && !isOrganizerArtifact(it->path())) files.push_back(it->path());
    }
}

// -------------------- HELP --------------------

void printHelp() {
    cout << "File Organizer v3.0\n";
    cout << "Usage: organizer <path> [options]\n\n";
    cout << "Options:\n";
    cout << "  --preview              Show what would be moved, no changes made\n";
    cout << "  --dry-run              Same as --preview\n";
    cout << "  --recursive            Process subdirectories recursively\n";
    cout << "  --by-name              Organize by filename parts instead of extension\n";
    cout << "  --max-depth <n>        Max folder depth for --by-name (default: 2)\n";
    cout << "  --on-duplicate <mode>  rename (default) | skip | overwrite\n";
    cout << "  --dedupe <mode>        Find byte-identical files by content: skip | report\n";
    cout << "  --min-size <size>      Skip files smaller than size (e.g. 1MB, 500KB)\n";
    cout << "  --max-size <size>      Skip files larger than size\n";
    cout << "  --newer-than <dur>     Only files modified within duration (e.g. 30d, 2w, 1y)\n";
    cout << "  --older-than <dur>     Only files modified before duration ago\n";
    cout << "  --undo <logfile>       Reverse a previous run using its .organizer_log.json\n";
    cout << "  --help                 Show this message\n\n";
    cout << "Config:\n";
    cout << "  Category overrides are read from ~/.organizer/config.json and from\n";
    cout << "  <path>/.organizer.json, e.g. {\"categories\": {\"psd\": \"Design\"}}. Both are\n";
    cout << "  optional and merge on top of the built-in extension->category defaults.\n\n";
    cout << "Examples:\n";
    cout << "  organizer ~/Downloads --recursive --preview\n";
    cout << "  organizer ~/Downloads --on-duplicate skip --min-size 1MB\n";
    cout << "  organizer ~/Downloads --newer-than 7d --by-name --max-depth 3\n";
    cout << "  organizer ~/Downloads --dedupe skip\n";
    cout << "  organizer ~/Downloads --undo ~/Downloads/.organizer_log.json\n";
}

// -------------------- MAIN --------------------

int main(int argc, char* argv[]) {
    if (argc < 2) { printHelp(); return 1; }

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printHelp(); return 0; }
        if (arg == "--undo" && i + 1 < argc) { performUndo(argv[i + 1]); return 0; }
    }

    fs::path path = argv[1];
    if (!fs::exists(path) || !fs::is_directory(path)) {
        cerr << "[FATAL] Invalid directory: " << path << "\n";
        return 1;
    }

    auto categories = loadCategories(path);

    bool preview  = false;
    bool recursive = false;
    bool byName   = false;
    int  maxDepth = 2;
    DuplicateStrategy dupStrategy = DuplicateStrategy::RENAME;
    DedupeStrategy dedupeStrategy = DedupeStrategy::NONE;
    long long minSize = -1, maxSize = -1;
    fs::file_time_type newerThan = fs::file_time_type::min();
    fs::file_time_type olderThan = fs::file_time_type::max();

    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if      (arg == "--preview" || arg == "--dry-run") preview = true;
        else if (arg == "--recursive")  recursive = true;
        else if (arg == "--by-name")    byName = true;
        else if (arg == "--max-depth"   && i + 1 < argc) maxDepth = parseMaxDepth(argv[++i], maxDepth);
        else if (arg == "--min-size"    && i + 1 < argc) minSize  = parseSize(argv[++i]);
        else if (arg == "--max-size"    && i + 1 < argc) maxSize  = parseSize(argv[++i]);
        else if (arg == "--newer-than"  && i + 1 < argc) newerThan = parseDuration(argv[++i]);
        else if (arg == "--older-than"  && i + 1 < argc) olderThan = parseDuration(argv[++i]);
        else if (arg == "--on-duplicate" && i + 1 < argc) {
            string mode = argv[++i];
            if      (mode == "skip")      dupStrategy = DuplicateStrategy::SKIP;
            else if (mode == "overwrite") dupStrategy = DuplicateStrategy::OVERWRITE;
            else if (mode == "rename")    dupStrategy = DuplicateStrategy::RENAME;
            else cerr << "[WARN] Unknown duplicate mode '" << mode << "', using rename\n";
        }
        else if (arg == "--dedupe" && i + 1 < argc) {
            string mode = argv[++i];
            if      (mode == "skip")   dedupeStrategy = DedupeStrategy::SKIP;
            else if (mode == "report") dedupeStrategy = DedupeStrategy::REPORT;
            else cerr << "[WARN] Unknown dedupe mode '" << mode << "', dedupe disabled\n";
        }
    }

    // -------- FILE COLLECTION --------
    vector<fs::path> files;
    try {
        auto opts = fs::directory_options::skip_permission_denied;
        error_code ec;
        if (recursive) {
            fs::recursive_directory_iterator it(path, opts, ec);
            if (ec) throw fs::filesystem_error("Cannot open directory", path, ec);
            collectFiles(move(it), files);
        } else {
            fs::directory_iterator it(path, opts, ec);
            if (ec) throw fs::filesystem_error("Cannot open directory", path, ec);
            collectFiles(move(it), files);
        }
    } catch (const exception& e) {
        cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    if (files.empty()) { cout << "No files found.\n"; return 0; }

    size_t dedupeCount = 0;
    if (dedupeStrategy != DedupeStrategy::NONE) {
        auto dedupe = findDuplicates(files, dedupeStrategy);
        dedupeCount = dedupe.duplicates.size();
        bool skipping = (dedupeStrategy == DedupeStrategy::SKIP);
        for (auto& [dup, original] : dedupe.duplicates) {
            cout << "[DEDUPE] " << dup.filename().string() << " duplicates " << original.filename().string()
                 << (skipping ? " (skipped)" : "") << "\n";
        }
        if (skipping) files = move(dedupe.keep);
    }

    if (files.empty()) { cout << "No files left to process after dedupe.\n"; return 0; }

    size_t total = files.size();
    cout << "Found " << total << " files. Processing...\n\n";

    size_t progressStep = max((size_t)1, total / 20);
    auto startTime = chrono::steady_clock::now();

    atomic<size_t> processed{0}, moved{0}, skipped{0}, filtered{0}, errors{0};
    mutex dirCacheMutex, dupMutex, logMutex;
    unordered_set<string> createdDirs, reservedTargets;
    unordered_map<string, int> pathCounters;

    WorkerContext ctx{
        files, categories, preview, recursive, byName, maxDepth, dupStrategy,
        minSize, maxSize, newerThan, olderThan, path, total, progressStep, startTime,
        processed, moved, skipped, filtered, errors,
        dirCacheMutex, createdDirs, dupMutex, pathCounters, reservedTargets, logMutex
    };

    unsigned int hw = thread::hardware_concurrency();
    unsigned int numThreads = hw == 0 ? 4 : hw;
    numThreads = (unsigned int)min<size_t>(numThreads, total);
    numThreads = max(numThreads, 1u);
    if (total < 64) numThreads = 1; // not worth spinning up threads for tiny runs

    atomic<size_t> nextIndex{0};
    vector<vector<MoveRecord>> perThreadLogs(numThreads);
    vector<thread> workers;
    for (unsigned int t = 0; t < numThreads; t++)
        workers.emplace_back(worker, ref(ctx), ref(nextIndex), ref(perThreadLogs[t]));
    for (auto& t : workers) t.join();

    vector<MoveRecord> undoLog;
    for (auto& log : perThreadLogs)
        undoLog.insert(undoLog.end(), make_move_iterator(log.begin()), make_move_iterator(log.end()));

    // -------- SUMMARY --------
    auto totalMs = chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now() - startTime).count();

        cout << "\n--- Done in " << totalMs << "ms ---\n";
        if (preview) cout << "  Would move: " << (total - filtered.load() - skipped.load()) << "\n";
        else         cout << "  Moved:      " << moved.load()    << "\n";
        if (skipped.load())  cout << "  Skipped:    " << skipped.load()  << "\n";
        if (filtered.load()) cout << "  Filtered:   " << filtered.load() << " (size/date criteria)\n";
        if (dedupeCount)     cout << "  Duplicates: " << dedupeCount << (dedupeStrategy == DedupeStrategy::SKIP ? " (skipped)\n" : " (reported)\n");
        if (errors.load())   cerr << "  Errors:     " << errors.load()   << "\n";

        if (!preview && !undoLog.empty()) writeUndoLog(path, undoLog);

        return (errors.load() > 0) ? 1 : 0;
}
