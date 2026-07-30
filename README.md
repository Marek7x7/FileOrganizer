# File Organizer Tool

## Overview  
A command-line file organizer that automatically categorizes files into folders based on their extensions or filename patterns. Supports both extension-based sorting and name-based folder hierarchy creation.

---

## Compilation

Using CMake (recommended, cross-platform):
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
The `organizer` executable is written to `build/`. This also builds `organizer_tests`; run the
suite with `ctest --test-dir build` or `cd build && ctest`.

Or compile directly with g++:
```bash
g++ -std=c++17 -O2 -pthread -o organizer main.cpp
```

---

## Usage
./organizer.exe <directory_path> [options]

--preview              Show what would be moved, no changes made

--dry-run              Same as --preview

--recursive            Process subdirectories recursively

--by-name              Organize by filename parts instead of extension

--max-depth <n>        Max folder depth for --by-name (default: 2)

--on-duplicate <mode>  rename (default) | skip | overwrite

--dedupe <mode>        Find byte-identical files by content: skip | report

--min-size <size>      Skip files smaller than size (e.g. 1MB, 500KB)

--max-size <size>      Skip files larger than size

--newer-than <dur>     Only files modified within duration (e.g. 30d, 2w, 1y)

--older-than <dur>     Only files modified before duration ago

--undo <logfile>       Reverse a previous run using its .organizer_log.json

--help                 Show this message

### Examples:
    organizer ~/Downloads --recursive --preview
    organizer ~/Downloads --on-duplicate skip --min-size 1MB
    organizer ~/Downloads --newer-than 7d --by-name --max-depth 3
    organizer ~/Downloads --dedupe skip
    organizer ~/Downloads --undo ~/Downloads/.organizer_log.json

---

## Category config

The built-in extension→category map can be extended or overridden without recompiling. Both
files are optional and merge on top of the defaults (later layers win):

1. `~/.organizer/config.json` (or `%USERPROFILE%\.organizer\config.json` on Windows) — user-wide overrides.
2. `<path>/.organizer.json` — overrides scoped to the directory being organized.

Either file can be a flat map or wrapped in a `"categories"` key:
```json
{ "categories": { "psd": "Design", "sketch": "Design" } }
```

---


## Features
- Automatic file categorization by extension
- Extension→category map is configurable via JSON (see "Category config" above)
- Smart folder hierarchy from filename patterns
- Duplicate file handling (by filename)
- Byte-identical duplicate detection by content (`--dedupe`)
- Recursive directory support
- Preview mode for testing
- Able to undo changes
- Organize by file size
- Organize by file age

---

## Notes

- Requires C++17 compiler support
- Works with linux and Windows
- For name-based sorting, files must have meaningful patterns in their filenames