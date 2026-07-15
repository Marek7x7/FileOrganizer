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
using namespace std;

// -------------------- ENUMS --------------------

enum class DuplicateStrategy { RENAME, SKIP, OVERWRITE };

// -------------------- CONFIG --------------------

unordered_map<string, string> loadConfig() {
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
                              unordered_map<string, int>& pathCounters,
                              const unordered_set<string>& reservedTargets) {
    string key = targetPath.string();
    if (!fs::exists(targetPath) && !reservedTargets.count(key)) return targetPath;
    if (!pathCounters.count(key)) pathCounters[key] = 1;

    string stem = targetPath.stem().string();
    string ext  = targetPath.extension().string();
    fs::path parent = targetPath.parent_path();

    while (true) {
        fs::path candidate = parent / (stem + "(" + to_string(pathCounters[key]++) + ")" + ext);
        if (!fs::exists(candidate) && !reservedTargets.count(candidate.string())) return candidate;
    }
}

// Resolves and reserves a target path for `file` under `targetDir`, synchronizing
// across worker threads so no two threads settle on the same destination.
// Returns nullopt if the duplicate strategy is SKIP and a collision was found.
// The actual fs::rename happens outside the lock this function holds internally.
optional<fs::path> resolveTargetPath(const fs::path& targetDir, const fs::path& file,
                                      DuplicateStrategy dupStrategy, mutex& dupMutex,
                                      unordered_map<string, int>& pathCounters,
                                      unordered_set<string>& reservedTargets) {
    fs::path targetPath = targetDir / file.filename();
    lock_guard<mutex> lock(dupMutex);

    bool collision = fs::exists(targetPath) || reservedTargets.count(targetPath.string());
    if (collision) {
        if (dupStrategy == DuplicateStrategy::SKIP) return nullopt;
        if (dupStrategy == DuplicateStrategy::RENAME)
            targetPath = getUniquePathLocked(targetPath, pathCounters, reservedTargets);
        // OVERWRITE: keep targetPath as-is; fs::rename will replace it
    }

    reservedTargets.insert(targetPath.string());
    return targetPath;
}

// -------------------- NOISE WORDS --------------------

const unordered_set<string> NOISE_WORDS = {
    "final", "draft", "copy", "backup", "temp", "old", "new",
    "revised", "edit", "wip", "done", "review", "version",
    "v", "release", "build"
};

bool isNoiseToken(const string& token) {
    string lower = token;
    transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (NOISE_WORDS.count(lower)) return true;
    if (all_of(token.begin(), token.end(), ::isdigit)) return true;
    if (!token.empty() && (token[0] == 'v' || token[0] == 'V')) {
        string rest = token.substr(1);
        if (!rest.empty() && all_of(rest.begin(), rest.end(), [](char c){ return isdigit(c) || c == '.'; }))
            return true;
    }
    return false;
}

string titleCase(const string& s) {
    if (s.empty()) return s;
    string r = s;
    r[0] = toupper(r[0]);
    return r;
}

// Splits on _, -, space, and camelCase boundaries
vector<string> getNameParts(const fs::path& file) {
    string stem = file.stem().string();
    vector<string> raw;
    string current;

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

    vector<string> parts;
    for (const string& token : raw)
        if (!isNoiseToken(token)) parts.push_back(titleCase(token));
        return parts;
}

// -------------------- SIZE PARSING --------------------

long long parseSize(const string& s) {
    string upper = s;
    transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    long long mult = 1;
    string digits = s;

    if (upper.size() >= 2 && upper.back() == 'B') {
        char unit = upper[upper.size() - 2];
        if      (unit == 'K') { mult = 1024LL;              digits = s.substr(0, s.size() - 2); }
        else if (unit == 'M') { mult = 1024LL * 1024;        digits = s.substr(0, s.size() - 2); }
        else if (unit == 'G') { mult = 1024LL * 1024 * 1024; digits = s.substr(0, s.size() - 2); }
        else                  {                               digits = s.substr(0, s.size() - 1); }
    }

    try { return stoll(digits) * mult; }
    catch (...) { cerr << "[ERROR] Invalid size: " << s << "\n"; return -1; }
}

// -------------------- DURATION PARSING --------------------

// Returns the point in time = now - duration, used as a filter cutoff
fs::file_time_type parseDuration(const string& s) {
    if (s.empty()) return fs::file_time_type::min();
    int value = 0;
    try { value = stoi(s); } catch (...) {
        cerr << "[ERROR] Invalid duration: " << s << "\n";
        return fs::file_time_type::min();
    }
    char unit = tolower(s.back());
    chrono::seconds offset;
    switch (unit) {
        case 'd': offset = chrono::hours(24 * value);          break;
        case 'w': offset = chrono::hours(24 * 7 * value);      break;
        case 'm': offset = chrono::hours(24 * 30 * value);     break;
        case 'y': offset = chrono::hours(24 * 365 * value);    break;
        default:
            cerr << "[ERROR] Unknown duration unit '" << unit << "' — use d/w/m/y\n";
            return fs::file_time_type::min();
    }
    return fs::file_time_type::clock::now() - offset;
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
        for (char c : s) out += (c == '\\') ? "\\\\" : (c == '"') ? "\\\"" : string(1, c);
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

    auto unescape = [](const string& s) {
        string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                if (s[i+1] == '\\') { out += '\\'; ++i; }
                else if (s[i+1] == '"') { out += '"'; ++i; }
                else out += s[i];
            } else { out += s[i]; }
        }
        return out;
    };

    auto extract = [](const string& line, const string& key) -> string {
        size_t pos = line.find(key);
        if (pos == string::npos) return "";
        size_t start = pos + key.size();
        size_t end = line.find("\"", start);
        return (end == string::npos) ? "" : line.substr(start, end - start);
    };

    string line;
    int restored = 0, failed = 0;
    while (getline(log, line)) {
        string from = extract(line, "\"from\": \"");
        string to   = extract(line, "\"to\": \"");
        if (from.empty() || to.empty()) continue;
        from = unescape(from);
        to   = unescape(to);
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
        cout << fixed << setprecision(1)
             << "  " << pct << "% (" << p << "/" << ctx.total
             << ") [" << ms << "ms]\n";
    }
}

void processFile(const fs::path& file, WorkerContext& ctx, vector<MoveRecord>& localUndoLog) {
    size_t p = ctx.processed.fetch_add(1, memory_order_relaxed) + 1;

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
        needsCreate = !ctx.createdDirs.count(dirKey);
    }
    if (needsCreate) {
        if (!fs::exists(targetDir)) fs::create_directories(targetDir);
        lock_guard<mutex> lock(ctx.dirCacheMutex);
        ctx.createdDirs.insert(dirKey);
    }

    // ---- DUPLICATE HANDLING ----
    auto resolved = resolveTargetPath(targetDir, file, ctx.dupStrategy, ctx.dupMutex,
                                       ctx.pathCounters, ctx.reservedTargets);
    if (!resolved) {
        {
            lock_guard<mutex> lock(ctx.logMutex);
            cout << "[SKIP] " << file.filename().string() << " (already exists)\n";
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
            cout << "[PREVIEW] " << file.filename().string()
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
            cerr << "[ERROR] " << file.filename().string() << ": " << e.what() << "\n";
            ctx.errors.fetch_add(1, memory_order_relaxed);
        }
    }

    maybePrintProgress(ctx, p);
}

void worker(WorkerContext& ctx, atomic<size_t>& nextIndex, vector<MoveRecord>& localUndoLog) {
    size_t idx;
    while ((idx = nextIndex.fetch_add(1, memory_order_relaxed)) < ctx.total) {
        processFile(ctx.files[idx], ctx, localUndoLog);
    }
}

// -------------------- HELP --------------------

void printHelp() {
    cout << "File Organizer v2.0\n";
    cout << "Usage: organizer <path> [options]\n\n";
    cout << "Options:\n";
    cout << "  --preview              Show what would be moved, no changes made\n";
    cout << "  --dry-run              Same as --preview\n";
    cout << "  --recursive            Process subdirectories recursively\n";
    cout << "  --by-name              Organize by filename parts instead of extension\n";
    cout << "  --max-depth <n>        Max folder depth for --by-name (default: 2)\n";
    cout << "  --on-duplicate <mode>  rename (default) | skip | overwrite\n";
    cout << "  --min-size <size>      Skip files smaller than size (e.g. 1MB, 500KB)\n";
    cout << "  --max-size <size>      Skip files larger than size\n";
    cout << "  --newer-than <dur>     Only files modified within duration (e.g. 30d, 2w, 1y)\n";
    cout << "  --older-than <dur>     Only files modified before duration ago\n";
    cout << "  --undo <logfile>       Reverse a previous run using its .organizer_log.json\n";
    cout << "  --help                 Show this message\n\n";
    cout << "Examples:\n";
    cout << "  organizer ~/Downloads --recursive --preview\n";
    cout << "  organizer ~/Downloads --on-duplicate skip --min-size 1MB\n";
    cout << "  organizer ~/Downloads --newer-than 7d --by-name --max-depth 3\n";
    cout << "  organizer ~/Downloads --undo ~/Downloads/.organizer_log.json\n";
}

// -------------------- MAIN --------------------

int main(int argc, char* argv[]) {
    auto categories = loadConfig();

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

    bool preview  = false;
    bool recursive = false;
    bool byName   = false;
    int  maxDepth = 2;
    DuplicateStrategy dupStrategy = DuplicateStrategy::RENAME;
    long long minSize = -1, maxSize = -1;
    fs::file_time_type newerThan = fs::file_time_type::min();
    fs::file_time_type olderThan = fs::file_time_type::max();

    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if      (arg == "--preview" || arg == "--dry-run") preview = true;
        else if (arg == "--recursive")  recursive = true;
        else if (arg == "--by-name")    byName = true;
        else if (arg == "--max-depth"   && i + 1 < argc) maxDepth = stoi(argv[++i]);
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
    }

    // -------- FILE COLLECTION --------
    vector<fs::path> files;
    try {
        if (recursive)
            for (const auto& e : fs::recursive_directory_iterator(path))
            { if (fs::is_regular_file(e)) files.push_back(e.path()); }
            else
                for (const auto& e : fs::directory_iterator(path))
                { if (fs::is_regular_file(e)) files.push_back(e.path()); }
    } catch (const exception& e) {
        cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    if (files.empty()) { cout << "No files found.\n"; return 0; }

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
        if (errors.load())   cerr << "  Errors:     " << errors.load()   << "\n";

        if (!preview && !undoLog.empty()) writeUndoLog(path, undoLog);

        return (errors.load() > 0) ? 1 : 0;
}
