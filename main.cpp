#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace std;

// Category system 
unordered_map<string, string> loadConfig(const string& filename) {
    unordered_map<string, string> extToCategory;
    ifstream file(filename);

    if (!file) {
        cout << "Failed to open config file.\n";
        return extToCategory;
    }

    string line;
    while (getline(file, line)) {
        size_t colon = line.find(':');
        if (colon == string::npos) continue;

        string category = line.substr(0, colon);
        string extensions = line.substr(colon + 1);

        istringstream iss(extensions);
        string ext;

        while (iss >> ext) {
            transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            extToCategory[ext] = category;
        }
    }

    return extToCategory;
}

// filename generator
fs::path getUniquePath(const fs::path& targetPath) {
    if (!fs::exists(targetPath)) return targetPath;

    string stem = targetPath.stem().string();
    string ext = targetPath.extension().string();
    fs::path parent = targetPath.parent_path();

    int counter = 1;
    while (true) {
        fs::path newPath = parent / (stem + "(" + to_string(counter) + ")" + ext);
        if (!fs::exists(newPath)) {
            return newPath;
        }
        counter++;
    }
}

// Split filename into parts by '_' and spaces
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
    if (!current.empty()) {
        parts.push_back(current);
    }

    return parts;
}

int main(int argc, char* argv[]) {
    auto categories = loadConfig("config.txt");
    if (argc < 2) {
        cout << "Usage: organizer.exe <path> [--preview] [--recursive] [--by-name] [--max-depth <n>]\n";
        return 1;
    }

    fs::path path = argv[1];

    bool preview = false;
    bool recursive = false;
    bool byName = false;
    int maxDepth = 2; // Default max depth for name-based sorting

    // Parse flags
    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--preview") preview = true;
        if (arg == "--recursive") recursive = true;
        if (arg == "--by-name") byName = true;
        if (arg == "--max-depth" && i + 1 < argc) {
            maxDepth = stoi(argv[++i]);
        }
    }

    if (!fs::exists(path) || !fs::is_directory(path)) {
        cout << "Invalid directory.\n";
        return 1;
    }

    vector<fs::path> files;

    try {
        
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (fs::is_regular_file(entry)) {
                    files.push_back(entry.path());
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (fs::is_regular_file(entry)) {
                    files.push_back(entry.path());
                }
            }
        }

        for (const auto& file : files) {
            fs::path targetDir;

            if (byName) {
                // Build nested folder path from name parts
                vector<string> parts = getNameParts(file);

                // Truncate to maxDepth (default 2, override with --max-depth)
                if ((int)parts.size() > maxDepth) {
                    parts.resize(maxDepth);
                }

                // Drop trailing part if it's a random index (number with fewer than 4 digits)
                if (!parts.empty()) {
                    const string& last = parts.back();
                    bool isNum = !last.empty() && all_of(last.begin(), last.end(), ::isdigit);
                    if (isNum && last.size() < 4) {
                        parts.pop_back();
                    }
                }

                if (parts.empty()) {
                    targetDir = recursive ? file.parent_path() / "Other" : path / "Other";
                } else {
                    targetDir = recursive ? file.parent_path() : path;
                    for (const string& part : parts) {
                        targetDir = targetDir / part;
                    }
                }
            } else {
                // Extension-based sorting
                string ext = file.extension().string();
                transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                string category = "Other";
                if (categories.count(ext)) {
                    category = categories[ext];
                }

                if (recursive) {
                    targetDir = file.parent_path() / category;
                } else {
                    targetDir = path / category;
                }
            }

            // Create all nested directories if needed
            if (!fs::exists(targetDir)) {
                fs::create_directories(targetDir);
            }

            fs::path targetPath = targetDir / file.filename();
            targetPath = getUniquePath(targetPath);

            if (preview) {
                cout << "[PREVIEW] " << file << " -> " << targetPath << "\n";
            } else {
                try {
                    fs::rename(file, targetPath);
                    cout << "Moved: " << file.filename()
                         << " -> " << targetPath << "\n";
                } catch (const exception& e) {
                    cout << "Failed: " << file << " | " << e.what() << "\n";
                }
            }
        }

    } catch (const exception& e) {
        cout << "Error: " << e.what() << "\n";
    }

    return 0;
}
