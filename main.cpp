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

namespace fs = std::filesystem;

// -------------------- ENUMS --------------------

enum class DuplicateStrategy { RENAME, SKIP, OVERWRITE };

// -------------------- CONFIG --------------------

std::unordered_map<std::string, std::string> loadConfig() {
    return {
        {"pdf",  "Documents"}, {"docx", "Documents"}, {"txt",  "Documents"},
        {"xlsx", "Documents"}, {"json", "Documents"}, {"md",   "Documents"},
        {"jpg",  "Images"},    {"png",  "Images"},    {"gif",  "Images"},
        {"bmp",  "Images"},    {"jpeg", "Images"},
        {"cpp",  "Code"},      {"h",    "Code"},      {"py",   "Code"},
        {"js",   "Code"},      {"ts",   "Code"},      {"java", "Code"},
        {"csv",  "Data"},      {"xml",  "Data"},      {"yaml", "Data"},
        {"mp4",  "Videos"},    {"mkv",  "Videos"},
    };
}

// -------------------- FILE UTILS --------------------

// Caller must hold dupMutex. reservedTargets tracks paths other threads have
// already committed to but not yet physically moved into place, so two
// threads never pick the same "unique" target for a fs::rename race.
fs::path getUniquePathLocked(const fs::path& targetPath,
    std::unordered_map<std::string, int>& pathCounters,
    const std::unordered_set<std::string>& reservedTargets) {
    std::string key = targetPath.string();
    if (!fs::exists(targetPath) && !reservedTargets.count(key)) return targetPath;
    if (!pathCounters.count(key)) pathCounters[key] = 1;

    std::string stem = targetPath.stem().string();
    std::string ext  = targetPath.extension().string();
    fs::path parent = targetPath.parent_path();

    while (true) {
        fs::path candidate = parent / (stem + "(" + std::to_string(pathCounters[key]++) + ")" + ext);
        if (!fs::exists(candidate) && !reservedTargets.count(candidate.string())) return candidate;
    }
}

// Resolves and reserves a target path for `file` under `targetDir`, synchronizing
// across worker threads so no two threads settle on the same destination.
// Returns std::nullopt if the duplicate strategy is SKIP and a collision was found.
// The actual fs::rename happens outside the lock this function holds internally.
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

// -------------------- NOISE WORDS --------------------

const std::unordered_set<std::string> NOISE_WORDS = {
    "final", "draft", "copy", "backup", "temp", "old", "new",
    "revised", "edit", "wip", "done", "review", "version",
    "v", "release", "build"
};

bool isNoiseToken(const std::string& token) {
    std::string lower = token;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (NOISE_WORDS.count(lower)) return true;
    if (std::all_of(token.begin(), token.end(), ::isdigit)) return true;
    if (!token.empty() && (token[0] == 'v' || token[0] == 'V')) {
        std::string rest = token.substr(1);
        if (!rest.empty() && std::all_of(rest.begin(), rest.end(), [](char c){ return isdigit(c) || c == '.'; }))
            return true;
    }
    return false;
}

std::string titleCase(const std::string& s) {
    if (s.empty()) return s;
    std::string r = s;
    r[0] = toupper(r[0]);
    return r;
}

// Splits on _, -, space, and camelCase boundaries
std::vector<std::string> getNameParts(const fs::path& file) {
    std::string stem = file.stem().string();
    std::vector<std::string> raw;
    std::string current;

    for (size_t i = 0; i < stem.size(); ++i) {
        char c = stem[i];
        if (c == '_' || c == '-' || c == ' ') {
            if (!current.empty()) { raw.push_back(current); current.clear(); }
        } else if (i > 0 && isupper(c) && islower(stem[i - 1])) {
            if (!current.empty()) { raw.push_back(current); current.clear(); }
            current += c;
        } else {
            current += c;
        }
    }
    if (!current.empty()) raw.push_back(current);

    std::vector<std::string> parts;
    for (const std::string& token : raw) {
        if (!isNoiseToken(token)) parts.push_back(titleCase(token));
    }
    return parts;
}

// -------------------- SIZE PARSING --------------------

long long parseSize(const std::string& s) {
    std::string upper = s;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    long long mult = 1;
    std::string digits = s;

    if (upper.size() >= 2 && upper.back() == 'B') {
        char unit = upper[upper.size() - 2];
        if      (unit == 'K') { mult = 1024LL;              digits = s.substr(0, s.size() - 2); }
        else if (unit == 'M') { mult = 1024LL * 1024;        digits = s.substr(0, s.size() - 2); }
        else if (unit == 'G') { mult = 1024LL * 1024 * 1024; digits = s.substr(0, s.size() - 2); }
        else                  {                               digits = s.substr(0, s.size() - 1); }
    }

    try { return std::stoll(digits) * mult; }
    catch (...) { std::cerr << "[ERROR] Invalid size: " << s << "\n"; return -1; }
}

// -------------------- DURATION PARSING --------------------

// Returns the point in time = now - duration, used as a filter cutoff
fs::file_time_type parseDuration(const std::string& s) {
    if (s.empty()) return fs::file_time_type::min();
    int value = 0;
    try { value = std::stoi(s); } catch (...) {
        std::cerr << "[ERROR] Invalid duration: " << s << "\n";
        return fs::file_time_type::min();
    }
    char unit = tolower(s.back());
    std::chrono::seconds offset;
    switch (unit) {
        case 'd': offset = std::chrono::hours(24 * value);          break;
        case 'w': offset = std::chrono::hours(24 * 7 * value);      break;
        case 'm': offset = std::chrono::hours(24 * 30 * value);     break;
        case 'y': offset = std::chrono::hours(24 * 365 * value);    break;
        default:
            std::cerr << "[ERROR] Unknown duration unit '" << unit << "' — use d/w/m/y\n";
            return fs::file_time_type::min();
    }
    return fs::file_time_type::clock::now() - offset;
}

// -------------------- UNDO LOG --------------------

struct MoveRecord { std::string from, to; };

void writeUndoLog(const fs::path& rootDir, const std::vector<MoveRecord>& moves) {
    if (moves.empty()) return;
    fs::path logPath = rootDir / ".organizer_log.json";

    std::time_t t = time(nullptr);
    std::tm* ti = std::localtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ti);

    std::ofstream log(logPath);
    if (!log) { std::cerr << "[WARN] Could not write undo log to " << logPath << "\n"; return; }

    auto escape = [](const std::string& s) {
        std::string out;
        for (char c : s) out += (c == '\\') ? "\\\\" : (c == '"') ? "\\\"" : std::string(1, c);
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
    std::cout << "Undo log: " << logPath << "\n";
}

void performUndo(const fs::path& logPath) {
    std::ifstream log(logPath);
    if (!log) { std::cerr << "[FATAL] Cannot open log: " << logPath << "\n"; return; }

    auto unescape = [](const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                if (s[i+1] == '\\') { out += '\\'; ++i; }
                else if (s[i+1] == '"') { out += '"'; ++i; }
                else out += s[i];
            } else { out += s[i]; }
        }
        return out;
    };

    auto extract = [](const std::string& line, const std::string& key) -> std::string {
        size_t pos = line.find(key);
        if (pos == std::string::npos) return "";
        size_t start = pos + key.size();
        size_t end = line.find("\"", start);
        return (end == std::string::npos) ? "" : line.substr(start, end - start);
    };

    std::string line;
    int restored = 0, failed = 0;
    while (std::getline(log, line)) {
        std::string from = extract(line, "\"from\": \"");
        std::string to   = extract(line, "\"to\": \"");
        if (from.empty() || to.empty()) continue;
        from = unescape(from);
        to   = unescape(to);
        try {
            if (fs::exists(to)) {
                fs::rename(to, from);
                std::cout << "Restored: " << to << "\n        -> " << from << "\n";
                restored++;
            } else {
                std::cerr << "[SKIP] No longer exists: " << to << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] " << e.what() << "\n";
            failed++;
        }
    }
    std::cout << "\nUndo complete: " << restored << " restored, " << failed << " failed\n";
}

// -------------------- WORKER (multithreaded file processing) --------------------

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
    std::atomic<size_t>& previewed;

    std::mutex& dirCacheMutex;
    std::unordered_set<std::string>& createdDirs;
    std::mutex& dupMutex;
    std::unordered_map<std::string, int>& pathCounters;
    std::unordered_set<std::string>& reservedTargets;
    std::mutex& logMutex;
};

void maybePrintProgress(WorkerContext& ctx, size_t p) {
    if (p % ctx.progressStep == 0 || p == ctx.total) {
        double pct = (double)p / ctx.total * 100.0;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ctx.startTime).count();
        std::lock_guard<std::mutex> lock(ctx.logMutex);
        // \r + clear-to-end-of-line redraws this line in place instead of scrolling;
        // no trailing '\n' so the next progress update overwrites it again.
        std::cout << "\r\033[K" << std::fixed << std::setprecision(1)
             << "  " << pct << "% (" << p << "/" << ctx.total
             << ") [" << ms << "ms]" << std::flush;
    }
}

void processFile(const fs::path& file, WorkerContext& ctx, std::vector<MoveRecord>& localUndoLog) {
    size_t p = ctx.processed.fetch_add(1, std::memory_order_relaxed) + 1;

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
            std::cout << "\r\033[K[SKIP] " << file.filename().string() << " (already exists)\n";
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
            std::cout << "\r\033[K[PREVIEW] " << file.filename().string()
                 << "\n          -> " << targetPath << "\n";
        }
        // Previewed files are NOT skipped files; count them separately so the
        // summary doesn't subtract every previewed file back out to zero.
        ctx.previewed.fetch_add(1, std::memory_order_relaxed);
    } else {
        try {
            // OVERWRITE: fs::rename onto an existing file is implementation-defined
            // (works on POSIX, unreliable on Windows). Remove the existing target
            // first so behavior is the same on every platform.
            if (ctx.dupStrategy == DuplicateStrategy::OVERWRITE && fs::exists(targetPath)) {
                fs::remove(targetPath);
            }

            std::error_code ec;
            fs::rename(file, targetPath, ec);

            if (ec == std::errc::cross_device_link) {
                // rename() can't move across filesystems/mount points (e.g. a
                // Downloads folder and destination on different drives).
                // Fall back to copy-then-delete instead of silently failing.
                fs::copy_file(file, targetPath, fs::copy_options::overwrite_existing, ec);
                if (!ec) fs::remove(file, ec);
            }

            if (ec) {
                std::lock_guard<std::mutex> lock(ctx.logMutex);
                std::cerr << "\r\033[K[ERROR] " << file.filename().string() << ": " << ec.message() << "\n";
                ctx.errors.fetch_add(1, std::memory_order_relaxed);
            } else {
                localUndoLog.push_back({file.string(), targetPath.string()});
                ctx.moved.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(ctx.logMutex);
            std::cerr << "\r\033[K[ERROR] " << file.filename().string() << ": " << e.what() << "\n";
            ctx.errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    maybePrintProgress(ctx, p);
}

void worker(WorkerContext& ctx, std::atomic<size_t>& nextIndex, std::vector<MoveRecord>& localUndoLog) {
    size_t idx;
    while ((idx = nextIndex.fetch_add(1, std::memory_order_relaxed)) < ctx.total) {
        processFile(ctx.files[idx], ctx, localUndoLog);
    }
}

// -------------------- HELP --------------------

void printHelp() {
    std::cout << "File Organizer v2.0\n";
    std::cout << "Usage: organizer <path> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --preview              Show what would be moved, no changes made\n";
    std::cout << "  --dry-run              Same as --preview\n";
    std::cout << "  --recursive            Process subdirectories recursively\n";
    std::cout << "  --by-name              Organize by filename parts instead of extension\n";
    std::cout << "  --max-depth <n>        Max folder depth for --by-name (default: 2)\n";
    std::cout << "  --on-duplicate <mode>  rename (default) | skip | overwrite\n";
    std::cout << "  --min-size <size>      Skip files smaller than size (e.g. 1MB, 500KB)\n";
    std::cout << "  --max-size <size>      Skip files larger than size\n";
    std::cout << "  --newer-than <dur>     Only files modified within duration (e.g. 30d, 2w, 1y)\n";
    std::cout << "  --older-than <dur>     Only files modified before duration ago\n";
    std::cout << "  --undo <logfile>       Reverse a previous run using its .organizer_log.json\n";
    std::cout << "  --help                 Show this message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  organizer ~/Downloads --recursive --preview\n";
    std::cout << "  organizer ~/Downloads --on-duplicate skip --min-size 1MB\n";
    std::cout << "  organizer ~/Downloads --newer-than 7d --by-name --max-depth 3\n";
    std::cout << "  organizer ~/Downloads --undo ~/Downloads/.organizer_log.json\n";
}

// -------------------- MAIN --------------------

int main(int argc, char* argv[]) {
    auto categories = loadConfig();

    if (argc < 2) { printHelp(); return 1; }

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printHelp(); return 0; }
        if (arg == "--undo" && i + 1 < argc) { performUndo(argv[i + 1]); return 0; }
    }

    fs::path path = argv[1];
    if (!fs::exists(path) || !fs::is_directory(path)) {
        std::cerr << "[FATAL] Invalid directory: " << path << "\n";
        return 1;
    }

    bool preview  = false;
    bool recursive = false;
    bool byName   = false;
    int  maxDepth = 2;
    DuplicateStrategy dupStrategy = DuplicateStrategy::RENAME;
    long long minSize = -1, maxSize = -1;
    fs::file_time_type newerThan = fs::file_time_type::min();
    fs::file_time_type olderThan = fs::file_time_type::max();

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--preview" || arg == "--dry-run") preview = true;
        else if (arg == "--recursive")  recursive = true;
        else if (arg == "--by-name")    byName = true;
        else if (arg == "--max-depth"   && i + 1 < argc) {
            try { maxDepth = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "[WARN] Invalid --max-depth value '" << argv[i] << "', using " << maxDepth << "\n"; }
        }
        else if (arg == "--min-size"    && i + 1 < argc) minSize  = parseSize(argv[++i]);
        else if (arg == "--max-size"    && i + 1 < argc) maxSize  = parseSize(argv[++i]);
        else if (arg == "--newer-than"  && i + 1 < argc) newerThan = parseDuration(argv[++i]);
        else if (arg == "--older-than"  && i + 1 < argc) olderThan = parseDuration(argv[++i]);
        else if (arg == "--on-duplicate" && i + 1 < argc) {
            std::string mode = argv[++i];
            if      (mode == "skip")      dupStrategy = DuplicateStrategy::SKIP;
            else if (mode == "overwrite") dupStrategy = DuplicateStrategy::OVERWRITE;
            else if (mode == "rename")    dupStrategy = DuplicateStrategy::RENAME;
            else std::cerr << "[WARN] Unknown duplicate mode '" << mode << "', using rename\n";
        }
    }

    // -------- FILE COLLECTION --------
    std::vector<fs::path> files;
    try {
        if (recursive)
            for (const auto& e : fs::recursive_directory_iterator(path))
            { if (fs::is_regular_file(e)) files.push_back(e.path()); }
            else
                for (const auto& e : fs::directory_iterator(path))
                { if (fs::is_regular_file(e)) files.push_back(e.path()); }
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    if (files.empty()) { std::cout << "No files found.\n"; return 0; }

    size_t total = files.size();
    std::cout << "Found " << total << " files. Processing...\n\n";

    size_t progressStep = std::max((size_t)1, total / 20);
    auto startTime = std::chrono::steady_clock::now();

    std::atomic<size_t> processed{0}, moved{0}, skipped{0}, filtered{0}, errors{0}, previewed{0};
    std::mutex dirCacheMutex, dupMutex, logMutex;
    std::unordered_set<std::string> createdDirs, reservedTargets;
    std::unordered_map<std::string, int> pathCounters;

    WorkerContext ctx{
        files, categories, preview, recursive, byName, maxDepth, dupStrategy,
        minSize, maxSize, newerThan, olderThan, path, total, progressStep, startTime,
        processed, moved, skipped, filtered, errors, previewed,
        dirCacheMutex, createdDirs, dupMutex, pathCounters, reservedTargets, logMutex
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

    // -------- SUMMARY --------
    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

        std::cout << "\n--- Done in " << totalMs << "ms ---\n";
        if (preview) std::cout << "  Would move: " << previewed.load() << "\n";
        else         std::cout << "  Moved:      " << moved.load()    << "\n";
        if (skipped.load())  std::cout << "  Skipped:    " << skipped.load()  << " (already exists)\n";
        if (filtered.load()) std::cout << "  Filtered:   " << filtered.load() << " (size/date criteria)\n";
        if (errors.load())   std::cerr << "  Errors:     " << errors.load()   << "\n";

        if (!preview && !undoLog.empty()) writeUndoLog(path, undoLog);

        return (errors.load() > 0) ? 1 : 0;
}