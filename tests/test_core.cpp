#include "mini_test.h"
#include "organizer_core.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// -------------------- isNoiseToken --------------------

TEST_CASE("isNoiseToken recognizes noise words case-insensitively") {
    CHECK(isNoiseToken("final"));
    CHECK(isNoiseToken("Final"));
    CHECK(isNoiseToken("DRAFT"));
    CHECK(!isNoiseToken("Report"));
}

TEST_CASE("isNoiseToken recognizes all-digit tokens") {
    CHECK(isNoiseToken("2023"));
    CHECK(!isNoiseToken("2023a"));
}

TEST_CASE("isNoiseToken recognizes version-like tokens") {
    CHECK(isNoiseToken("v2"));
    CHECK(isNoiseToken("v2.1"));
    CHECK(isNoiseToken("V3"));
    CHECK(!isNoiseToken("vase")); // starts with v but isn't a version token
}

// -------------------- getNameParts --------------------

TEST_CASE("getNameParts splits on separators and filters noise") {
    auto parts = getNameParts(fs::path("Invoice_Final_2023.pdf"));
    CHECK_EQ(parts.size(), (size_t)1);
    if (!parts.empty()) CHECK_EQ(parts[0], "Invoice");
}

TEST_CASE("getNameParts splits camelCase boundaries") {
    auto parts = getNameParts(fs::path("myCamelCaseFile.txt"));
    std::vector<std::string> expected = {"My", "Camel", "Case", "File"};
    CHECK_EQ(parts.size(), expected.size());
    for (size_t i = 0; i < parts.size() && i < expected.size(); ++i) CHECK_EQ(parts[i], expected[i]);
}

TEST_CASE("getNameParts returns empty when everything is noise") {
    auto parts = getNameParts(fs::path("v2_final.txt"));
    CHECK(parts.empty());
}

// -------------------- parseSize --------------------

TEST_CASE("parseSize handles plain byte counts") {
    CHECK_EQ(parseSize("1024"), 1024LL);
}

TEST_CASE("parseSize handles K/M/G suffixes") {
    CHECK_EQ(parseSize("1KB"), 1024LL);
    CHECK_EQ(parseSize("1MB"), 1024LL * 1024);
    CHECK_EQ(parseSize("1GB"), 1024LL * 1024 * 1024);
    CHECK_EQ(parseSize("500KB"), 500LL * 1024);
}

TEST_CASE("parseSize returns -1 on invalid input") {
    CHECK_EQ(parseSize("not-a-size"), -1LL);
}

// -------------------- parseMaxDepth --------------------

TEST_CASE("parseMaxDepth parses valid non-negative integers") {
    CHECK_EQ(parseMaxDepth("3", 2), 3);
    CHECK_EQ(parseMaxDepth("0", 2), 0);
}

TEST_CASE("parseMaxDepth falls back on invalid or negative input") {
    CHECK_EQ(parseMaxDepth("abc", 2), 2);
    CHECK_EQ(parseMaxDepth("-1", 2), 2);
}

// -------------------- parseDuration --------------------

TEST_CASE("parseDuration returns min() for empty or unknown-unit input") {
    CHECK(parseDuration("") == fs::file_time_type::min());
    CHECK(parseDuration("10x") == fs::file_time_type::min());
}

TEST_CASE("parseDuration orders shorter durations closer to now") {
    // "1 day ago" is a later (larger) time_point than "2 days ago".
    auto oneDayAgo = parseDuration("1d");
    auto twoDaysAgo = parseDuration("2d");
    CHECK(oneDayAgo > twoDaysAgo);
}

// -------------------- getUniquePathLocked --------------------

TEST_CASE("getUniquePathLocked returns the original path when it doesn't exist") {
    fs::path dir = fs::temp_directory_path() / "organizer_test_unique_1";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::unordered_map<std::string, int> counters;
    std::unordered_set<std::string> reserved;
    fs::path target = dir / "report.pdf";

    fs::path result = getUniquePathLocked(target, counters, reserved);
    CHECK_EQ(result.string(), target.string());

    fs::remove_all(dir);
}

TEST_CASE("getUniquePathLocked numbers around existing files and reservations") {
    fs::path dir = fs::temp_directory_path() / "organizer_test_unique_2";
    fs::remove_all(dir);
    fs::create_directories(dir);

    fs::path target = dir / "report.pdf";
    { std::ofstream(target) << "x"; } // pre-existing file on disk

    std::unordered_map<std::string, int> counters;
    std::unordered_set<std::string> reserved;

    fs::path first = getUniquePathLocked(target, counters, reserved);
    CHECK_EQ(first.string(), (dir / "report(1).pdf").string());

    // Simulate another thread having already reserved report(1).pdf.
    reserved.insert(first.string());
    fs::path second = getUniquePathLocked(target, counters, reserved);
    CHECK_EQ(second.string(), (dir / "report(2).pdf").string());

    fs::remove_all(dir);
}

// -------------------- isOrganizerArtifact --------------------

TEST_CASE("isOrganizerArtifact recognizes the organizer's own config and log files") {
    CHECK(isOrganizerArtifact(fs::path("/some/dir/.organizer.json")));
    CHECK(isOrganizerArtifact(fs::path("/some/dir/.organizer_log.json")));
    CHECK(!isOrganizerArtifact(fs::path("/some/dir/notes.json")));
    CHECK(!isOrganizerArtifact(fs::path("/some/dir/report.pdf")));
}

// -------------------- parseJson --------------------

TEST_CASE("parseJson parses a flat string object") {
    JsonValue v = parseJson(R"({"pdf": "Documents", "jpg": "Images"})");
    CHECK(v.isObject);
    CHECK_EQ(v.members.size(), (size_t)2);
    CHECK_EQ(v.members["pdf"].stringValue, "Documents");
    CHECK_EQ(v.members["jpg"].stringValue, "Images");
}

TEST_CASE("parseJson parses nested objects") {
    JsonValue v = parseJson(R"({"categories": {"psd": "Design"}})");
    CHECK(v.isObject);
    CHECK(v.members["categories"].isObject);
    CHECK_EQ(v.members["categories"].members["psd"].stringValue, "Design");
}

TEST_CASE("parseJson handles string escapes") {
    JsonValue v = parseJson(R"({"a": "line1\nline2\ttabbed\"quoted\""})");
    CHECK_EQ(v.members["a"].stringValue, "line1\nline2\ttabbed\"quoted\"");
}

TEST_CASE("parseJson throws on malformed input") {
    bool threw = false;
    try { parseJson(R"({"a": })"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// -------------------- mergeCategoryConfig --------------------

TEST_CASE("mergeCategoryConfig merges a flat document") {
    std::unordered_map<std::string, std::string> categories = {{"pdf", "Documents"}};
    mergeCategoryConfig(categories, parseJson(R"({"psd": "Design"})"));
    CHECK_EQ(categories["pdf"], "Documents");
    CHECK_EQ(categories["psd"], "Design");
}

TEST_CASE("mergeCategoryConfig merges a nested categories document and overrides existing keys") {
    std::unordered_map<std::string, std::string> categories = {{"pdf", "Documents"}};
    mergeCategoryConfig(categories, parseJson(R"({"categories": {"pdf": "Docs2", "psd": "Design"}})"));
    CHECK_EQ(categories["pdf"], "Docs2");
    CHECK_EQ(categories["psd"], "Design");
}

// -------------------- loadCategoryConfigFile --------------------

TEST_CASE("loadCategoryConfigFile ignores a missing file") {
    std::unordered_map<std::string, std::string> categories = {{"pdf", "Documents"}};
    loadCategoryConfigFile(fs::temp_directory_path() / "organizer_test_does_not_exist.json", categories);
    CHECK_EQ(categories.size(), (size_t)1);
    CHECK_EQ(categories["pdf"], "Documents");
}

TEST_CASE("loadCategoryConfigFile merges a real file and leaves the map untouched on parse error") {
    fs::path dir = fs::temp_directory_path() / "organizer_test_config";
    fs::remove_all(dir);
    fs::create_directories(dir);

    fs::path good = dir / "good.json";
    { std::ofstream(good) << R"({"categories": {"psd": "Design"}})"; }

    std::unordered_map<std::string, std::string> categories = {{"pdf", "Documents"}};
    loadCategoryConfigFile(good, categories);
    CHECK_EQ(categories["psd"], "Design");
    CHECK_EQ(categories["pdf"], "Documents");

    fs::path bad = dir / "bad.json";
    { std::ofstream(bad) << "not json at all"; }
    size_t before = categories.size();
    loadCategoryConfigFile(bad, categories);
    CHECK_EQ(categories.size(), before); // unchanged, just logs an [ERROR]

    fs::remove_all(dir);
}

// -------------------- findDuplicates --------------------

TEST_CASE("findDuplicates with NONE leaves the file list untouched") {
    std::vector<fs::path> files = {"a.txt", "b.txt"};
    auto result = findDuplicates(files, DedupeStrategy::NONE);
    CHECK_EQ(result.keep.size(), files.size());
    CHECK(result.duplicates.empty());
}

TEST_CASE("findDuplicates treats the oldest file as the original, not the alphabetically-first one") {
    fs::path dir = fs::temp_directory_path() / "organizer_test_dedupe";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // "copy.txt" sorts before "original.txt" alphabetically, but it's the
    // newer file -- the original/duplicate distinction should follow mtime,
    // not path string order.
    fs::path original = dir / "original.txt";
    fs::path copy = dir / "copy.txt";
    fs::path other = dir / "unique.txt";
    { std::ofstream(original) << "hello world"; }
    { std::ofstream(copy) << "hello world"; } // identical content to `original`
    { std::ofstream(other) << "something else"; }

    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(original, now - std::chrono::seconds(60));
    fs::last_write_time(copy, now);

    std::vector<fs::path> files = {original, copy, other};

    auto reportResult = findDuplicates(files, DedupeStrategy::REPORT);
    CHECK_EQ(reportResult.keep.size(), (size_t)3); // report mode never removes files
    CHECK_EQ(reportResult.duplicates.size(), (size_t)1);
    if (!reportResult.duplicates.empty()) {
        CHECK_EQ(reportResult.duplicates[0].first.string(), copy.string());      // newer file is the duplicate
        CHECK_EQ(reportResult.duplicates[0].second.string(), original.string()); // older file is the original
    }

    auto skipResult = findDuplicates(files, DedupeStrategy::SKIP);
    CHECK_EQ(skipResult.keep.size(), (size_t)2); // copy removed, original and other kept
    bool copyStillPresent = std::find(skipResult.keep.begin(), skipResult.keep.end(), copy) != skipResult.keep.end();
    CHECK(!copyStillPresent);

    fs::remove_all(dir);
}
