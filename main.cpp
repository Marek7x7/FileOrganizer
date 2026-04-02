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

// Duplicate-safe filename generator
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

int main(int argc, char* argv[]) {
    auto categories = loadConfig("config.txt");
    if (argc < 2) {
        cout << "Usage: organizer.exe <path> [--preview] [--recursive]\n";
        return 1;
    }

    fs::path path = argv[1];

    bool preview = false;
    bool recursive = false;

    // Parse flags
    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--preview") preview = true;
        if (arg == "--recursive") recursive = true;
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
            string ext = file.extension().string();
            transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            string category = "Other";
            if (categories.count(ext)) {
                category = categories[ext];
            }
            fs::path categoryPath = path / category;

            if (!fs::exists(categoryPath)) {
                fs::create_directory(categoryPath);
            }

            fs::path targetPath = categoryPath / file.filename();
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