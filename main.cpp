#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace std;

// -------------------- UTIL --------------------

string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t");
    return (start == string::npos) ? "" : s.substr(start, end - start + 1);
}

// -------------------- CONFIG --------------------

unordered_map<string, string> loadConfig(const string& filename) {
    unordered_map<string, string> extToCategory;
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

vector<string> getNameParts(const fs::path& file) {
    string stem = file.stem().string();
    vector<string> parts;
    string current;

    for (char c : stem) {
        if (c == '_' || c == ' ') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    if (!current.empty()) parts.push_back(current);
    return parts;
}

// -------------------- MAIN --------------------

int main(int argc, char* argv[]) {

    // -------- CONFIG RESOLUTION --------
    vector<string> configCandidates;

    char* envConfig = std::getenv("ORGANIZER_CONFIG");
    if (envConfig) {
        cout << "[DEBUG] ENV config detected: " << envConfig << "\n";
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
        return 1;
    }

    cout << "[DEBUG] Using config: " << configPath << "\n";

    auto categories = loadConfig(configPath);

    if (categories.empty()) {
        cout << "[FATAL] Config loaded but contains no valid mappings.\n";
        return 1;
    }

    // -------- ARGUMENTS --------
    if (argc < 2) {
        cout << "Usage: organizer.exe <path> [--preview] [--recursive] [--by-name] [--max-depth <n>]\n";
        return 1;
    }

    fs::path path = argv[1];

    if (!fs::exists(path) || !fs::is_directory(path)) {
        cout << "[FATAL] Invalid directory.\n";
        return 1;
    }

    bool preview = false;
    bool recursive = false;
    bool byName = false;
    int maxDepth = 2;

    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--preview") preview = true;
        else if (arg == "--recursive") recursive = true;
        else if (arg == "--by-name") byName = true;
        else if (arg == "--max-depth" && i + 1 < argc) {
            maxDepth = stoi(argv[++i]);
        }
    }

    // -------- FILE COLLECTION --------
    vector<fs::path> files;

    try {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (fs::is_regular_file(entry)) files.push_back(entry.path());
            }
        } else {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (fs::is_regular_file(entry)) files.push_back(entry.path());
            }
        }

        // -------- PROCESS FILES --------
        for (const auto& file : files) {

            fs::path targetDir;

            if (byName) {
                vector<string> parts = getNameParts(file);

                if ((int)parts.size() > maxDepth) {
                    parts.resize(maxDepth);
                }

                if (!parts.empty()) {
                    const string& last = parts.back();
                    bool isNum = all_of(last.begin(), last.end(), ::isdigit);
                    if (isNum && last.size() < 4) {
                        parts.pop_back();
                    }
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

    } catch (const exception& e) {
        cout << "[FATAL] " << e.what() << "\n";
    }

    return 0;
}