#pragma once

// Pure, side-effect-free (or purely local-state) helper functions used by the
// organizer CLI, split out from main.cpp so they can be unit tested without
// linking in the multithreaded worker / main() machinery.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// -------------------- CONFIG --------------------

inline std::unordered_map<std::string, std::string> loadConfig() {
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

// -------------------- JSON CONFIG --------------------

// Deliberately not a general-purpose JSON library: this reads only what the
// organizer's config files need (nested objects with string leaf values —
// no arrays, numbers, bools, or null). Malformed input throws
// std::runtime_error with a human-readable message; callers treat that as a
// soft failure (log and fall back to defaults), same as the other parseX
// helpers in this file.
struct JsonValue {
    bool isObject = false;
    std::string stringValue;
    std::map<std::string, JsonValue> members;
};

namespace json_detail {

class Parser {
public:
    explicit Parser(const std::string& text) : s(text) {}

    JsonValue parseValue() {
        skipWs();
        if (pos >= s.size()) fail("unexpected end of input");
        char c = s[pos];
        if (c == '{') return parseObject();
        if (c == '"') {
            JsonValue v;
            v.isObject = false;
            v.stringValue = parseString();
            return v;
        }
        fail(std::string("unexpected character '") + c + "'");
        return {}; // unreachable; fail() throws
    }

private:
    const std::string& s;
    size_t pos = 0;

    [[noreturn]] void fail(const std::string& msg) {
        throw std::runtime_error("JSON parse error at offset " + std::to_string(pos) + ": " + msg);
    }

    void skipWs() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) ++pos;
    }

    void expect(char c) {
        skipWs();
        if (pos >= s.size() || s[pos] != c) fail(std::string("expected '") + c + "'");
        ++pos;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (pos >= s.size()) fail("unterminated string");
            char c = s[pos++];
            if (c == '"') break;
            if (c != '\\') { out += c; continue; }

            if (pos >= s.size()) fail("unterminated escape");
            char e = s[pos++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    if (pos + 4 > s.size()) fail("invalid unicode escape");
                    unsigned code = (unsigned)std::stoul(s.substr(pos, 4), nullptr, 16);
                    pos += 4;
                    // BMP-only, encoded as UTF-8 (surrogate pairs aren't needed
                    // for the ASCII category names/extensions this reads).
                    if (code < 0x80) {
                        out += (char)code;
                    } else if (code < 0x800) {
                        out += (char)(0xC0 | (code >> 6));
                        out += (char)(0x80 | (code & 0x3F));
                    } else {
                        out += (char)(0xE0 | (code >> 12));
                        out += (char)(0x80 | ((code >> 6) & 0x3F));
                        out += (char)(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: fail("invalid escape character");
            }
        }
        return out;
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue obj;
        obj.isObject = true;
        skipWs();
        if (pos < s.size() && s[pos] == '}') { ++pos; return obj; }
        while (true) {
            skipWs();
            std::string key = parseString();
            expect(':');
            obj.members[key] = parseValue();
            skipWs();
            if (pos >= s.size()) fail("unterminated object");
            if (s[pos] == ',') { ++pos; continue; }
            if (s[pos] == '}') { ++pos; break; }
            fail("expected ',' or '}'");
        }
        return obj;
    }
};

} // namespace json_detail

inline JsonValue parseJson(const std::string& text) {
    json_detail::Parser p(text);
    return p.parseValue();
}

// Reads category overrides out of a parsed config document and merges them
// into `categories`, overwriting any existing entries with the same
// extension. Accepts either {"categories": {"ext": "cat", ...}} or a flat
// {"ext": "cat", ...} document with no wrapping key.
inline void mergeCategoryConfig(std::unordered_map<std::string, std::string>& categories,
    const JsonValue& root) {
    if (!root.isObject) return;
    const JsonValue* src = &root;
    auto it = root.members.find("categories");
    if (it != root.members.end() && it->second.isObject) src = &it->second;

    for (auto& [key, val] : src->members)
        if (!val.isObject) categories[key] = val.stringValue;
}

// Merges category overrides from a JSON file at `path` into `categories`, if
// the file exists. A missing file is the normal case (no config present) and
// is silently ignored; a malformed file logs an [ERROR] and is skipped,
// leaving `categories` as it was before this call.
inline void loadCategoryConfigFile(const fs::path& path,
    std::unordered_map<std::string, std::string>& categories) {
    if (!fs::exists(path)) return;
    std::ifstream in(path);
    if (!in) return;

    std::stringstream buf;
    buf << in.rdbuf();
    try {
        mergeCategoryConfig(categories, parseJson(buf.str()));
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to parse config " << path << ": " << e.what() << "\n";
    }
}

// Effective category map for a run: compiled-in defaults, then
// ~/.organizer/config.json (or %USERPROFILE%\.organizer\config.json on
// Windows) if present, then <targetDir>/.organizer.json if present — each
// layer overriding/extending the one before it. Works with zero config files
// present, so the binary keeps behaving exactly like v2.0 out of the box.
inline std::unordered_map<std::string, std::string> loadCategories(const fs::path& targetDir) {
    auto categories = loadConfig();

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home) loadCategoryConfigFile(fs::path(home) / ".organizer" / "config.json", categories);

    loadCategoryConfigFile(targetDir / ".organizer.json", categories);

    return categories;
}

// Filenames the organizer itself writes into a target directory (the
// per-directory config override and the undo log). These are excluded from
// file collection so a run never organizes its own config/log away —
// otherwise a second run in the same directory would silently lose the
// per-directory config or leave a stray undo log sitting in a category folder.
inline bool isOrganizerArtifact(const fs::path& file) {
    std::string name = file.filename().string();
    return name == ".organizer.json" || name == ".organizer_log.json";
}

// -------------------- FILE UTILS --------------------

// Caller must hold dupMutex. reservedTargets tracks paths other threads have
// already committed to but not yet physically moved into place, so two
// threads never pick the same "unique" target for a fs::rename race.
inline fs::path getUniquePathLocked(const fs::path& targetPath,
    std::unordered_map<std::string, int>& pathCounters,
    const std::unordered_set<std::string>& reservedTargets) {
    std::string key = targetPath.string();
    if (!fs::exists(targetPath) && !reservedTargets.count(key)) return targetPath;
    if (!pathCounters.count(key)) pathCounters[key] = 1;

    std::string stem = targetPath.stem().string();
    std::string ext  = targetPath.extension().string();
    fs::path parent = targetPath.parent_path();

    constexpr int kMaxAttempts = 100000;
    while (pathCounters[key] <= kMaxAttempts) {
        fs::path candidate = parent / (stem + "(" + std::to_string(pathCounters[key]++) + ")" + ext);
        if (!fs::exists(candidate) && !reservedTargets.count(candidate.string())) return candidate;
    }
    throw std::runtime_error("Could not find a unique name for " + targetPath.string() +
                              " after " + std::to_string(kMaxAttempts) + " attempts");
}

// -------------------- NOISE WORDS --------------------

inline const std::unordered_set<std::string> NOISE_WORDS = {
    "final", "draft", "copy", "backup", "temp", "old", "new",
    "revised", "edit", "wip", "done", "review", "version",
    "v", "release", "build"
};

inline bool isNoiseToken(const std::string& token) {
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

inline std::string titleCase(const std::string& s) {
    if (s.empty()) return s;
    std::string r = s;
    r[0] = toupper(r[0]);
    return r;
}

// Splits on _, -, space, and camelCase boundaries
inline std::vector<std::string> getNameParts(const fs::path& file) {
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
    for (const std::string& token : raw)
        if (!isNoiseToken(token)) parts.push_back(titleCase(token));
    return parts;
}

// -------------------- SIZE PARSING --------------------

inline long long parseSize(const std::string& s) {
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

// -------------------- MAX DEPTH PARSING --------------------

inline int parseMaxDepth(const std::string& s, int fallback) {
    try {
        int v = std::stoi(s);
        if (v < 0) throw std::invalid_argument("negative");
        return v;
    } catch (...) {
        std::cerr << "[ERROR] Invalid --max-depth value: " << s << " (using " << fallback << ")\n";
        return fallback;
    }
}

// -------------------- DURATION PARSING --------------------

// Returns the point in time = now - duration, used as a filter cutoff
inline fs::file_time_type parseDuration(const std::string& s) {
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

// -------------------- CONTENT DEDUPE --------------------

enum class DedupeStrategy { NONE, SKIP, REPORT };

// FNV-1a 64-bit hash of a file's contents, read in fixed-size chunks so large
// files don't need to be loaded into memory at once. Not cryptographic --
// this is only used to find accidentally-duplicated files, not to resist
// deliberate tampering.
inline uint64_t hashFileContents(const fs::path& file) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET;

    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open file for hashing: " + file.string());

    std::vector<char> buffer(1 << 16);
    while (true) {
        in.read(buffer.data(), (std::streamsize)buffer.size());
        std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= (uint64_t)(unsigned char)buffer[(size_t)i];
            hash *= FNV_PRIME;
        }
        if (got == 0 || !in) break;
    }
    return hash;
}

struct DedupeResult {
    std::vector<fs::path> keep;                                  // files list, duplicates removed if strategy == SKIP
    std::vector<std::pair<fs::path, fs::path>> duplicates;        // (duplicate, original) pairs found
};

// Groups `files` by size, then by content hash within each size group, to
// find byte-identical duplicates cheaply (files are only hashed if another
// file shares their exact size). Within each group of duplicates, the
// lexicographically-first path is treated as the "original"; the others are
// reported as duplicates of it. Files that can't be stat'd or read are left
// untouched (kept, not reported) so the normal per-file pass can report the
// real error against them.
inline DedupeResult findDuplicates(const std::vector<fs::path>& files, DedupeStrategy strategy) {
    DedupeResult result;
    if (strategy == DedupeStrategy::NONE) { result.keep = files; return result; }

    std::unordered_map<uintmax_t, std::vector<fs::path>> bySize;
    for (const auto& f : files) {
        std::error_code ec;
        uintmax_t sz = fs::file_size(f, ec);
        if (ec) { result.keep.push_back(f); continue; }
        bySize[sz].push_back(f);
    }

    std::unordered_set<std::string> duplicatePaths;

    for (auto& [size, group] : bySize) {
        (void)size;
        if (group.size() < 2) continue;

        std::unordered_map<uint64_t, std::vector<fs::path>> byHash;
        for (const auto& f : group) {
            try { byHash[hashFileContents(f)].push_back(f); }
            catch (...) { /* unreadable; left for the normal pass to report */ }
        }

        for (auto& [hash, hashGroup] : byHash) {
            (void)hash;
            if (hashGroup.size() < 2) continue;
            // The oldest file by modification time is treated as the "original";
            // path string is only a tie-break for determinism, not the primary
            // rule (alphabetical order says nothing about which file came first).
            std::sort(hashGroup.begin(), hashGroup.end(), [](const fs::path& a, const fs::path& b) {
                std::error_code ecA, ecB;
                auto timeA = fs::last_write_time(a, ecA);
                auto timeB = fs::last_write_time(b, ecB);
                if (!ecA && !ecB && timeA != timeB) return timeA < timeB;
                return a < b;
            });
            const fs::path& original = hashGroup.front();
            for (size_t i = 1; i < hashGroup.size(); ++i) {
                result.duplicates.push_back({hashGroup[i], original});
                duplicatePaths.insert(hashGroup[i].string());
            }
        }
    }

    for (const auto& f : files) {
        bool isDuplicate = duplicatePaths.count(f.string()) > 0;
        if (isDuplicate && strategy == DedupeStrategy::SKIP) continue;
        result.keep.push_back(f);
    }
    return result;
}
