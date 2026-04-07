# File Organizer Tool

## Overview  
A command-line file organizer that automatically categorizes files into folders based on their extensions or filename patterns. Supports both extension-based sorting and name-based folder hierarchy creation.

---

## Compilation  
```bash
g++ -std=c++17 -O2 -march=x86-64 -pthread -static-libgcc -static-libstdc++ -o organizer main.cpp
```

---

## Usage
./organizer.exe <directory_path> [options]

--recursive - Organize files recursively through subdirectories

--by-name - Create folder hierarchy based on filename patterns


--preview - Show preview of moves without actually moving files


--max-depth &lt;n&gt; - Set maximum depth for name-based folder hierarchy (default: 2)

#### ex: ./organizer.exe ~/Documents --recursive --by-name --max-depth 3

---

## configuration
Edit config.txt to define custom categories

Images: .png .jpg .jpeg  
Videos: .mp4 .mkv  
Code: .cpp .py .java .js  
Documents: .txt .pdf .md .docx .xlsx

---

## Features
- Automatic file categorization by extension
- Smart folder hierarchy from filename patterns
- Duplicate file handling
- Recursive directory support
- Preview mode for testing
- Customizable configuration

---

## Notes

- Requires C++17 compiler support
- Works with linux and Windows
- For name-based sorting, files must have meaningful patterns in their filenames
- The (--by-name) mode uses underscores and spaces as separators