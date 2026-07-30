# FileOrganizer v3.0 Roadmap

## Context

FileOrganizer is a single-file C++17 CLI (`main.cpp`, ~540 lines) currently at v2.0. It sorts files
either by extension (via a hardcoded `loadConfig()` map) or by filename-derived folder hierarchy
(`--by-name`), using a multithreaded worker pool with thread-safe duplicate-path resolution, size/date
filters, preview mode, and a hand-rolled JSON undo log. There's no external config, no content-based
classification, no true duplicate (by-content) detection, no build system beyond a raw `g++` command,
and no tests or CI.

Git history shows JSON config-file support existed once (`0fc07f8`) and was removed in a later refactor
(`38b28f3`) — restoring/improving on that is a real regression fix, not just a new idea.

This document is a prioritized improvement roadmap for v3.0, covering four areas: smarter sorting &
config, robustness & bug fixes, new CLI features, and build/test infra. It's a planning document —
nothing in `main.cpp` changes as part of it. Items are meant to be picked up individually in follow-up
work.

---

## 1. Smarter sorting & config

This is the highest-leverage area — it's the actual "make the algorithm smarter" ask.

1. **External category config** (restores the pre-`38b28f3` capability, done properly this time)
   - Move the `loadConfig()` extension→category map (`main.cpp:26-37`) out of code and into a JSON file,
     e.g. `~/.organizer/config.json` with an optional `.organizer.json` per-target-directory override
     that merges on top of the defaults.
   - Keep the current hardcoded map as the **compiled-in fallback** so the binary still works with zero
     config present — don't make config mandatory.
   - Needs a real (small, dependency-free or single-header) JSON parser rather than another hand-rolled
     line scanner like the undo log — see infra section for the same underlying need.

2. **Rule-based / regex categorization, evaluated before the extension map**
   - Let users define ordered rules like `{"pattern": "*invoice*.pdf", "category": "Finance/Invoices"}`
     matched against the filename (glob or regex) so e.g. all invoices land together regardless of
     extension, overriding the generic `Documents` bucket.
   - Rules run first; extension map is the fallback for anything unmatched — additive, not a rewrite of
     the existing `processFile` target-dir logic at `main.cpp:330-346`.

3. **True duplicate detection (content hash), not just name collision**
   - Today "duplicate" (`resolveTargetPath`, `main.cpp:65-84`) only means "something already occupies the
     target filename." Two files with different names but identical bytes are never detected.
   - Add an opt-in `--dedupe` pass: hash file contents (xxHash or SHA-1 is enough, not cryptographic
     strength needed) and offer strategies — skip the duplicate, hardlink it to the original, or just
     report it. This is genuinely smarter sorting, not cosmetic.
   - Cost-conscious: only hash when `--dedupe` is passed, and only hash files whose size matches another
     file already seen (size as a cheap pre-filter before reading bytes).

4. **Content sniffing (magic-byte detection) as a fallback for missing/wrong extensions**
   - When a file has no extension, or an extension not in the map, peek at the first few bytes
     (PDF `%PDF`, PNG/JPEG/ZIP signatures, etc.) to classify it instead of dumping everything into
     `Other`. Small, well-scoped win — a handful of signature checks cover the common cases.

5. **Better `--by-name` clustering**
   - Current `getNameParts` (`main.cpp:115-137`) splits into tokens and filters noise words, but two
     files like `Report_2024_Q1` and `Report_2024_Q2` already naturally land under `Report/2024/`, which
     is fine. The real gap: no date-pattern recognition (`2024-03-15`, `Mar2024`) to route into a
     `Year/Month` structure, and no near-duplicate stem clustering (e.g. `Invoice_Final` vs
     `Invoice_Final_2`) — could use a cheap similarity check (common-prefix length or Levenshtein on the
     filtered token list) to fold near-identical stems into the same folder instead of creating slightly
     different chains.
   - This one has the most ambiguity/judgment calls of the list — worth a short discussion before
     committing to a specific heuristic rather than guessing at what "smart" means here.

6. **Combined extension + name mode**
   - Currently `--by-name` and extension-based sorting are mutually exclusive (`ctx.byName` branch,
     `main.cpp:333-346`). Allow both together, e.g. `Documents/Invoice/...`, so extension gives the
     top-level bucket and name-parts refine within it.

## 2. Robustness & bug fixes

Concrete, low-risk fixes found while reading the code — worth doing regardless of what else ships.

1. **Uncaught-exception crash on worker threads** (`processFile`, `main.cpp:313-323`, `355`)
   `fs::file_size(file)`, `fs::last_write_time(file)`, and `fs::create_directories(targetDir)` are not
   wrapped in try/catch, unlike the `fs::rename` call a few lines below. If a file is deleted or becomes
   unreadable between directory listing and processing (race with another process, antivirus scan,
   in-progress download), the exception propagates out of a worker thread with no handler and calls
   `std::terminate`, killing the whole run instead of just skipping that file and incrementing `errors`.
2. **Fragile hand-rolled undo log** (`writeUndoLog`/`performUndo`, `main.cpp:188-263`)
   The writer does minimal escaping (backslash/quote only — no control characters) and the reader does a
   naive per-line substring scan for `"from": "` / `"to": "`. This breaks if a path ever contains that
   literal substring, if JSON formatting changes, or if a path contains a raw newline. Should either use
   a tiny embedded JSON writer/reader pair that's actually correct, or drop the JSON pretense and use a
   simpler robust format (NUL- or tab-separated with proper escaping) — same underlying parser work as
   the config-file need above, worth solving once and reusing.
3. **Unvalidated `--max-depth`** (`main.cpp:459`): `stoi(argv[++i])` is not wrapped in try/catch, unlike
   `parseSize`/`parseDuration` which are — a non-numeric value crashes the program instead of printing a
   clean `[ERROR]` and falling back to the default.
4. **Recursive scan aborts entirely on one permission error** (`main.cpp:476-481`): a single unreadable
   subdirectory during `recursive_directory_iterator` throws and is only caught by the top-level
   try/catch around the whole collection step, aborting the entire run. Should use the iterator overload
   that takes `fs::directory_options::skip_permission_denied` (or catch per-entry) so one bad subdirectory
   doesn't kill a scan of thousands of files.
5. **`getUniquePathLocked` unbounded loop** (`main.cpp:55-58`): loops forever incrementing a counter if it
   can never find a free name (e.g., persistent permission error on stat). Low risk but worth a sanity
   cap with a clear error instead of a silent hang.

## 3. New CLI features

1. **`--exclude <glob>` / `.organizerignore`** — skip matching files/directories (e.g. `.git`,
   `node_modules`) during collection, gitignore-style.
2. **Confirm prompt before moving** (or `--yes` to skip it) — moves aren't trivially reversible without
   the undo log; a default confirmation for non-preview runs adds a safety net for interactive use, with
   `--yes` for scripted/CI use.
3. **`--json` output** — machine-readable summary (counts, moves) for scripting, alongside the existing
   human-readable console output.
4. **`--log-file <path>` and verbosity flags** (`--quiet`, `--verbose`) — right now everything goes to
   stdout/stderr only.
5. **Watch mode** (`--watch`) — poll (or use inotify on Linux) a directory and auto-organize new files as
   they land. Meaningfully larger in scope than the rest of this list (daemon-like lifecycle, signal
   handling) — flag as a stretch goal / separate milestone rather than bundling it with the rest of v3.0.

## 4. Build & test infra

1. **`CMakeLists.txt`** — replace the ad hoc `g++ -std=c++17 -O2 -march=x86-64 -pthread` README command
   and the Windows-only MSYS2 path baked into `.vscode/tasks.json` with a real cross-platform build that
   works out of the box on Linux/macOS/Windows.
2. **Unit tests** — the pure helper functions are well-suited to testing and currently have zero
   coverage: `getNameParts`/`isNoiseToken` (`main.cpp:94-137`), `parseSize` (`main.cpp:141-158`),
   `parseDuration` (`main.cpp:163-182`), `getUniquePathLocked` (`main.cpp:44-59`). A lightweight
   header-only framework (doctest or Catch2) keeps this dependency-free.
3. **GitHub Actions CI** — build + run the new unit tests on push/PR across at least Linux (and ideally
   Windows, since the tool explicitly targets it).

---

## Suggested phasing

Given the four areas are all in scope, a sensible order (each phase independently shippable):

1. **Robustness fixes** (§2) — small, isolated, no design decisions, removes real crash risk. Do first.
2. **Build/test infra** (§4) — CMake + a test harness makes every subsequent change safer to land, so
   worth doing before the larger sorting/config work rather than after.
3. **Config file support + duplicate-content detection** (§1.1, §1.3) — the two highest-value, most
   self-contained "smarter sorting" wins; both need the same small JSON-parsing capability the undo-log
   fix also wants, so it pays for itself once.
4. **Rules engine, content sniffing, combined mode, better `--by-name` clustering** (§1.2, §1.4, §1.5,
   §1.6) — builds on the config-file work from step 3.
5. **New CLI features** (§3), with watch mode explicitly deferred/optional given its larger scope.

## Verification (for whichever phase gets implemented later)

- Build via the new `CMakeLists.txt` (or current `g++` command) on Linux and confirm no warnings with
  `-Wall -Wextra`.
- Run the unit tests once added (`ctest` or the chosen framework's runner).
- Manually exercise the CLI against a scratch directory: `--preview` first, then a real run, then
  `--undo` on the resulting `.organizer_log.json`, checking files land back exactly where they started.
- For robustness fixes specifically: reproduce each bug before fixing (e.g., delete a file mid-scan in a
  large directory to trigger the uncaught-exception path; pass `--max-depth abc`) and confirm graceful
  `[ERROR]` handling afterward instead of a crash.
