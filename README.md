# File Organizer Tool

## Overview
A command-line file organizer that automatically categorizes files into folders based on their extensions or filename patterns. Supports both extension-based sorting and name-based folder hierarchy creation, with multithreaded processing for large directories.

---

## Compilation
```bash
g++ -std=c++17 -O2 -march=x86-64 -pthread -o organizer main.cpp
```
Verified to compile cleanly with `-Wall -Wextra` and no warnings. On Windows the same command (with an appropriate toolchain) will produce `organizer.exe`.

---

## Usage
```
./organizer <directory_path> [options]
```

| Option | Description |
|---|---|
| `--preview` | Show what would be moved, no changes made |
| `--dry-run` | Same as `--preview` |
| `--recursive` | Process subdirectories recursively |
| `--by-name` | Organize by filename parts instead of extension |
| `--max-depth <n>` | Max folder depth for `--by-name` (default: 2) |
| `--on-duplicate <mode>` | `rename` (default) \| `skip` \| `overwrite` |
| `--min-size <size>` | Skip files smaller than size (e.g. `1MB`, `500KB`) |
| `--max-size <size>` | Skip files larger than size |
| `--newer-than <dur>` | Only files modified within duration (e.g. `30d`, `2w`, `1y`) |
| `--older-than <dur>` | Only files modified before duration ago |
| `--undo <logfile>` | Reverse a previous run using its `.organizer_log.json` |
| `--help` | Show this message |

### Examples
```
organizer ~/Downloads --recursive --preview
organizer ~/Downloads --on-duplicate skip --min-size 1MB
organizer ~/Downloads --newer-than 7d --by-name --max-depth 3
organizer ~/Downloads --undo ~/Downloads/.organizer_log.json
```

---

## Features
- Automatic file categorization by extension
- Smart folder hierarchy from filename patterns
- Duplicate file handling (rename / skip / overwrite)
- Recursive directory support
- Multithreaded processing (thread count scales with `hardware_concurrency()`; single-threaded for small runs to avoid overhead)
- Preview mode for testing
- Undo via a per-run JSON log
- Filtering by file size and file age
- Falls back to copy+delete when moving across filesystems/mount points (rename can't cross those)

---

## Known limitations
- `--by-name` combined with `--recursive` builds the name-derived folder hierarchy *inside each source subdirectory*, not flattened into the root — expect nested folders per directory, not one unified tree.
- The undo log is a small hand-written JSON writer/reader, not a full JSON parser. It handles the format this tool writes; don't hand-edit it with nested structures or embedded newlines in filenames.
- `--on-duplicate overwrite` deletes the existing destination file with no confirmation prompt. There's no recovery for the overwritten file — only for the file that replaced it (via `--undo`).
- Linux behavior is verified directly (compiled, run, and tested under ThreadSanitizer with 300+ files, zero data races). Windows compatibility is based on code review of `std::filesystem` semantics, not an actual Windows test run in this environment — treat that claim as unverified until someone runs it there.

---

## Notes
- Requires a C++17-capable compiler.
- For name-based sorting, files must have meaningful separators (`_`, `-`, space, or camelCase) in their filenames — a name with no recognizable parts falls into an `Other` folder.