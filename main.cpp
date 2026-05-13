// #include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;
using namespace std;

// -------------------- UTIL --------------------

string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t");
    return (start == string::npos) ? "" : s.substr(start, end - start + 1);
}

// -------------------- CONFIG --------------------

// Add JSON config support
bool loadConfigJSON(const string& filename, unordered_map<string, string>& extToCategory) {
    ifstream file(filename);
    if (!file) {
        return false;
    }

    string line;
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue; // Skip empty lines and comments
        
        // Simple JSON parsing for our use case
        size_t colon = line.find(':');
        if (colon == string::npos) continue;
        
        string category = trim(line.substr(0, colon));
        string extensions = trim(line.substr(colon + 1));
        
        // Remove quotes if present
        if (!extensions.empty() && (extensions.front() == '"' || extensions.front() == '\'')) {
            extensions = extensions.substr(1);
        }
        if (!extensions.empty() && (extensions.back() == '"' || extensions.back() == '\'')) {
            extensions.pop_back();
        }
        
        istringstream iss(extensions);
        string ext;
        while (iss >> ext) {
            transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            extToCategory[ext] = category;
        }
    }
    
    return true;
}

unordered_map<string, string> loadConfig(const string& filename) {
    unordered_map<string, string> extToCategory;
    
    // Try to load as JSON first
    if (loadConfigJSON(filename, extToCategory)) {
        return extToCategory;
    }
    
    // Fall back to text format
    ifstream file(filename);
    if (!file) {
        cout << "[FATAL] Failed to open config file: " << filename << "\n";
        return extToCategory;
    }

    string line;
    while (getline(file, line)) {
        if (line.empty()) continue;

        size_t colon = line.find(':');
        if (colon == string::npos) continue;

        string category = trim(line.substr(0, colon));
        string extensions = trim(line.substr(colon + 1));

        istringstream iss(extensions);
        string ext;

        while (iss >> ext) {
            transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            extToCategory[ext] = category;
        }
    }

    return extToCategory;
}

// -------------------- FILE UTILS --------------------

fs::path getUniquePath(const fs::path& targetPath) {
    if (!fs::exists(targetPath)) return targetPath;

    string stem = targetPath.stem().string();
    string ext = targetPath.extension().string();
    fs::path parent = targetPath.parent_path();

    int counter = 1;
    while (true) {
        fs::path newPath = parent / (stem + "(" + to_string(counter) + ")" + ext);
        if (!fs::exists(newPath)) return newPath;
        counter++;
    }
}

// Noise words that carry no organizational meaning
const unordered_map<string, bool> NOISE_WORDS = {
    {"final", true}, {"draft", true}, {"copy", true}, {"backup", true},
    {"temp", true},  {"old", true},   {"new", true},  {"revised", true},
    {"edit", true},  {"wip", true},   {"done", true},  {"review", true},
    {"version", true}, {"v", true}, {"release", true}, {"build", true}
};

bool isNoiseToken(const string& token) {
    // Lowercase version for comparison
    string lower = token;
    transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Known noise words
    if (NOISE_WORDS.count(lower)) return true;

    // Pure numeric sequences: counters (01, 002), years (2024), dates (20240315)
    bool allDigits = all_of(token.begin(), token.end(), ::isdigit);
    if (allDigits) return true;

    // Version strings: v1, v2, v1.0, v2.3.1
    if (!token.empty() && (token[0] == 'v' || token[0] == 'V')) {
        string rest = token.substr(1);
        bool looksLikeVersion = !rest.empty() &&
            all_of(rest.begin(), rest.end(), [](char c){ return isdigit(c) || c == '.'; });
        if (looksLikeVersion) return true;
    }

    return false;
}

string titleCase(const string& s) {
    if (s.empty()) return s;
    string result = s;
    result[0] = toupper(result[0]);
    return result;
}

vector<string> getNameParts(const fs::path& file) {
    string stem = file.stem().string();
    vector<string> raw;
    string current;

    // Split on _, -, and space
    for (char c : stem) {
        if (c == '_' || c == '-' || c == ' ') {
            if (!current.empty()) {
                raw.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) raw.push_back(current);

    // Filter noise tokens and apply title-case
    vector<string> parts;
    for (const string& token : raw) {
        if (!isNoiseToken(token)) {
            parts.push_back(titleCase(token));
        }
    }

    return parts;
}

// -------------------- HELP AND LOGGING --------------------

void printHelp() {
    cout << "File Organizer v1.0\n";
    cout << "Usage: organizer.exe <path> [--preview] [--recursive] [--by-name] [--max-depth <n>] [--dry-run]\n\n";
    cout << "Options:\n";
    cout << "  --preview    Show what would be moved without actually moving files\n";
    cout << "  --recursive  Process files in subdirectories recursively\n";
    cout << "  --by-name    Organize by file name parts instead of extension\n";
    cout << "  --max-depth <n>  Maximum depth for name-based organization (default: 2)\n";
    cout << "  --dry-run    Same as --preview (show what would be done)\n";
    cout << "  --help       Show this help message\n\n";
    cout << "Example:\n";
    cout << "  organizer.exe C:\\MyFiles --recursive --by-name --max-depth 3\n";
    cout << "  organizer.exe D:\\Documents --preview\n";
}

// -------------------- MAIN --------------------

int main(int argc, char* argv[]) {
    // -------- CONFIG RESOLUTION --------
    vector<string> configCandidates;

    char* envConfig = std::getenv("ORGANIZER_CONFIG");
    if (envConfig) {
        configCandidates.push_back(envConfig);
    }

    configCandidates.push_back("config.txt");
    configCandidates.push_back("config/config.txt");

    string configPath;

    for (const auto& path : configCandidates) {
        if (fs::exists(path)) {
            configPath = path;
            break;
        }
    }

    if (configPath.empty()) {
        cout << "[FATAL] No valid config file found.\n";
        cout << "Please create a config.txt file with extension mappings.\n";
        cout << "Example:\n";
        cout << "Documents: txt pdf doc docx\n";
        cout << "Images: jpg jpeg png gif bmp\n";
        cout << "Audio: mp3 wav flac\n";
        return 1;
    }

    auto categories = loadConfig(configPath);

    if (categories.empty()) {
        cout << "[FATAL] Config loaded but contains no valid mappings.\n";
        return 1;
    }

    // -------- ARGUMENTS --------
    if (argc < 2) {
        printHelp();
        return 1;
    }

    // Check for help flag
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        }
    }

    fs::path path = argv[1];

    if (!fs::exists(path) || !fs::is_directory(path)) {
        cout << "[FATAL] Invalid directory.\n";
        return 1;
    }

    bool preview = false;
    bool recursive = false;
    bool byName = false;
    bool dryRun = false;
    int maxDepth = 2;

    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--preview") preview = true;
        else if (arg == "--recursive") recursive = true;
        else if (arg == "--by-name") byName = true;
        else if (arg == "--max-depth" && i + 1 < argc) {
            maxDepth = stoi(argv[++i]);
        }
        else if (arg == "--dry-run") dryRun = true;
    }

    // Set preview mode if dry-run is specified
    if (dryRun) preview = true;

    // -------- FILE COLLECTION --------
    vector<fs::path> files;
    int totalFiles = 0;

    try {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (fs::is_regular_file(entry)) {
                    files.push_back(entry.path());
                    totalFiles++;
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (fs::is_regular_file(entry)) {
                    files.push_back(entry.path());
                    totalFiles++;
                }
            }
        }

        if (files.empty()) {
            cout << "No files found in directory.\n";
            return 0;
        }

        // -------- PROCESS FILES --------
        cout << "Processing " << totalFiles << " files...\n";
        
        // Progress tracking
        int processed = 0;
        auto startTime = chrono::steady_clock::now();
        
        for (const auto& file : files) {
            processed++;
            
            // Show progress every 100 files or for the last file
            if (processed % 100 == 0 || processed == totalFiles) {
                auto currentTime = chrono::steady_clock::now();
                auto duration = chrono::duration_cast<chrono::milliseconds>(currentTime - startTime).count();
                double progress = (double)processed / totalFiles * 100;
                cout << fixed << setprecision(1) << "Progress: " << progress << "% (" 
                     << processed << "/" << totalFiles << ") [" << duration << "ms]\n";
            }

            fs::path targetDir;

            if (byName) {
                // Filtering (noise removal, title-case) happens inside getNameParts.
                // maxDepth is applied AFTER filtering so noise tokens don't consume depth slots.
                vector<string> parts = getNameParts(file);

                if ((int)parts.size() > maxDepth) {
                    parts.resize(maxDepth);
                }

                if (parts.empty()) {
                    targetDir = recursive ? file.parent_path() / "Other" : path / "Other";
                } else {
                    targetDir = recursive ? file.parent_path() : path;
                    for (const string& part : parts) {
                        targetDir /= part;
                    }
                }

            } else {
                string ext = file.extension().string();

                if (!ext.empty() && ext[0] == '.') {
                    ext.erase(0, 1);
                }

                transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                string category = "Other";
                if (categories.count(ext)) {
                    category = categories[ext];
                }

                targetDir = recursive ? file.parent_path() / category : path / category;
            }

            if (!fs::exists(targetDir)) {
                fs::create_directories(targetDir);
            }

            fs::path targetPath = getUniquePath(targetDir / file.filename());

            if (preview) {
                cout << "[PREVIEW] " << file << " -> " << targetPath << "\n";
            } else {
                try {
                    fs::rename(file, targetPath);
                    cout << "Moved: " << file.filename() << " -> " << targetPath << "\n";
                } catch (const exception& e) {
                    cout << "[ERROR] " << file << " | " << e.what() << "\n";
                }
            }
        }

        auto endTime = chrono::steady_clock::now();
        auto duration = chrono::duration_cast<chrono::milliseconds>(endTime - startTime).count();
        cout << "Completed in " << duration << "ms\n";

    } catch (const exception& e) {
        cout << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
