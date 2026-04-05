#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>

namespace fs = std::filesystem;
using namespace std;

// Mutex for thread-safe output
std::mutex mtx;

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

void processChunk(const vector<fs::path>& chunk, const unordered_map<string, string>& categories, bool preview, bool recursive, const fs::path& base_path) {
    for (const auto& file : chunk) {
        try {
            string ext = file.extension().string();
            transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            string category = "Other";
            if (categories.count(ext)) {
                category = categories.at(ext); // Ensure the category is retrieved correctly
            }

            fs::path categoryPath;
            if (recursive) {
                categoryPath = file.parent_path() / category;
            } else {
                categoryPath = base_path / category;
            }

            if (!fs::exists(categoryPath)) {
                fs::create_directory(categoryPath);
            }

            fs::path targetPath = categoryPath / file.filename();
            targetPath = getUniquePath(targetPath);

            if (preview) {
                std::lock_guard<std::mutex> lock(mtx);
                std::cout << "[PREVIEW] " << file << " -> " << targetPath << "\n";
            } else {
                fs::rename(file, targetPath);
                std::lock_guard<std::mutex> lock(mtx);
                std::cout << "Moved: " << file.filename() << " -> " << targetPath << "\n";
            }
        } catch (const exception& e) {
            std::lock_guard<std::mutex> lock(mtx);
            std::cout << "Failed: " << file << " | " << e.what() << "\n";
        }
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

        // Determine number of threads (use 75% of available threads)
        int num_threads = std::thread::hardware_concurrency();
        if (num_threads <= 0) num_threads = 4; // fallback
        num_threads = std::max(1, num_threads * 3 / 4);

        // Split files into chunks
        vector<vector<fs::path>> chunks(num_threads);
        int chunk_size = files.size() / num_threads;
        int remainder = files.size() % num_threads;

        for (int i = 0; i < num_threads; ++i) {
            int start = i * chunk_size + std::min(i, remainder);
            int end = start + chunk_size + (i < remainder ? 1 : 0);
            chunks[i].assign(files.begin() + start, files.begin() + end);
        }

        // Create and join threads
        vector<thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back(processChunk, chunks[i], categories, preview, recursive, path);
        }

        for (auto& t : threads) {
            t.join();
        }

    } catch (const exception& e) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "Error: " << e.what() << "\n";
    }

    return 0;
}
