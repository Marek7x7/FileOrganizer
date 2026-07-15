# File Organizer Tool

## Overview  
A command-line file organizer that automatically categorizes files into folders based on their extensions or filename patterns. Supports both extension-based sorting and name-based folder hierarchy creation.

---

## Compilation  
```bash
g++ -std=c++17 -O2 -march=x86-64 -pthread -o organizer main.cpp
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
    organizer ~/Downloads --undo ~/Downloads/.organizer_log.json


---


## Features
- Automatic file categorization by extension
- Smart folder hierarchy from filename patterns
- Duplicate file handling
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
