#include <iostream>
#include <iomanip>
#include <filesystem>
#include <string>

#include "organizer_engine.h"

using namespace std;

// -------------------- HELP --------------------

void printHelp() {
    cout << "File Organizer v4.0\n";
    cout << "Usage: organizer <path> [options]\n\n";
    cout << "Options:\n";
    cout << "  --preview              Show what would be moved, no changes made\n";
    cout << "  --dry-run              Same as --preview\n";
    cout << "  --recursive            Process subdirectories recursively\n";
    cout << "  --by-name              Organize by filename parts instead of extension\n";
    cout << "  --max-depth <n>        Max folder depth for --by-name (default: 2)\n";
    cout << "  --on-duplicate <mode>  rename (default) | skip | overwrite\n";
    cout << "  --dedupe <mode>        Find byte-identical files by content: skip | report\n";
    cout << "  --min-size <size>      Skip files smaller than size (e.g. 1MB, 500KB)\n";
    cout << "  --max-size <size>      Skip files larger than size\n";
    cout << "  --newer-than <dur>     Only files modified within duration (e.g. 30d, 2w, 1y)\n";
    cout << "  --older-than <dur>     Only files modified before duration ago\n";
    cout << "  --undo <logfile>       Reverse a previous run using its .organizer_log.json\n";
    cout << "  --help                 Show this message\n\n";
    cout << "Config:\n";
    cout << "  Category overrides are read from ~/.organizer/config.json and from\n";
    cout << "  <path>/.organizer.json, e.g. {\"categories\": {\"psd\": \"Design\"}}. Both are\n";
    cout << "  optional and merge on top of the built-in extension->category defaults.\n\n";
    cout << "Examples:\n";
    cout << "  organizer ~/Downloads --recursive --preview\n";
    cout << "  organizer ~/Downloads --on-duplicate skip --min-size 1MB\n";
    cout << "  organizer ~/Downloads --newer-than 7d --by-name --max-depth 3\n";
    cout << "  organizer ~/Downloads --dedupe skip\n";
    cout << "  organizer ~/Downloads --undo ~/Downloads/.organizer_log.json\n";
}

// -------------------- CLI EVENT SINK --------------------

// Renders engine events to stdout/stderr exactly as this CLI has always
// printed them (progress redraw, [PREVIEW]/[SKIP]/[ERROR]/[DEDUPE] lines).
EventSink makeCliSink() {
    return [](const OrganizerEvent& e) {
        switch (e.kind) {
            case EventKind::Progress: {
                double pct = e.total ? (double)e.processed / e.total * 100.0 : 0.0;
                cout << "\r\033[K" << fixed << setprecision(1)
                     << "  " << pct << "% (" << e.processed << "/" << e.total
                     << ") [" << e.elapsedMs << "ms]" << flush;
                break;
            }
            case EventKind::Preview:
                cout << "\r\033[K[PREVIEW] " << e.path.filename().string()
                     << "\n          -> " << e.targetPath << "\n";
                break;
            case EventKind::Skip:
                cout << "\r\033[K[SKIP] " << e.path.filename().string() << " " << e.message << "\n";
                break;
            case EventKind::Error:
                cerr << "\r\033[K[ERROR] " << e.path.filename().string() << ": " << e.message << "\n";
                break;
            case EventKind::Dedupe:
                cout << "[DEDUPE] " << e.path.filename().string() << " duplicates "
                     << e.targetPath.filename().string() << e.message << "\n";
                break;
            case EventKind::Warning:
                cerr << e.message << "\n";
                break;
            case EventKind::Info:
                cout << e.message << "\n";
                break;
        }
    };
}

// -------------------- MAIN --------------------

int main(int argc, char* argv[]) {
    if (argc < 2) { printHelp(); return 1; }

    EventSink cliSink = makeCliSink();

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printHelp(); return 0; }
        if (arg == "--undo" && i + 1 < argc) { performUndo(argv[i + 1], cliSink); return 0; }
    }

    fs::path path = argv[1];
    if (!fs::exists(path) || !fs::is_directory(path)) {
        cerr << "[FATAL] Invalid directory: " << path << "\n";
        return 1;
    }

    OrganizeOptions opts;
    opts.path = path;

    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if      (arg == "--preview" || arg == "--dry-run") opts.preview = true;
        else if (arg == "--recursive")  opts.recursive = true;
        else if (arg == "--by-name")    opts.byName = true;
        else if (arg == "--max-depth"   && i + 1 < argc) opts.maxDepth = parseMaxDepth(argv[++i], opts.maxDepth);
        else if (arg == "--min-size"    && i + 1 < argc) opts.minSize  = parseSize(argv[++i]);
        else if (arg == "--max-size"    && i + 1 < argc) opts.maxSize  = parseSize(argv[++i]);
        else if (arg == "--newer-than"  && i + 1 < argc) opts.newerThan = parseDuration(argv[++i]);
        else if (arg == "--older-than"  && i + 1 < argc) opts.olderThan = parseDuration(argv[++i]);
        else if (arg == "--on-duplicate" && i + 1 < argc) {
            string mode = argv[++i];
            if      (mode == "skip")      opts.dupStrategy = DuplicateStrategy::SKIP;
            else if (mode == "overwrite") opts.dupStrategy = DuplicateStrategy::OVERWRITE;
            else if (mode == "rename")    opts.dupStrategy = DuplicateStrategy::RENAME;
            else cerr << "[WARN] Unknown duplicate mode '" << mode << "', using rename\n";
        }
        else if (arg == "--dedupe" && i + 1 < argc) {
            string mode = argv[++i];
            if      (mode == "skip")   opts.dedupeStrategy = DedupeStrategy::SKIP;
            else if (mode == "report") opts.dedupeStrategy = DedupeStrategy::REPORT;
            else cerr << "[WARN] Unknown dedupe mode '" << mode << "', dedupe disabled\n";
        }
    }

    OrganizeResult result = runOrganize(opts, cliSink);

    if (!result.ok) {
        cerr << "[FATAL] " << result.fatalError << "\n";
        return 1;
    }
    if (result.total == 0) return 0;

    // -------- SUMMARY --------
    cout << "\n--- Done in " << result.elapsedMs << "ms ---\n";
    if (result.preview) cout << "  Would move: " << (result.total - result.filtered - result.skipped) << "\n";
    else                 cout << "  Moved:      " << result.moved << "\n";
    if (result.skipped)    cout << "  Skipped:    " << result.skipped << "\n";
    if (result.filtered)   cout << "  Filtered:   " << result.filtered << " (size/date criteria)\n";
    if (result.duplicates) cout << "  Duplicates: " << result.duplicates
        << (opts.dedupeStrategy == DedupeStrategy::SKIP ? " (skipped)\n" : " (reported)\n");
    if (result.errors)     cerr << "  Errors:     " << result.errors << "\n";

    if (!opts.preview && !result.undoLog.empty()) writeUndoLog(path, result.undoLog, cliSink);

    return (result.errors > 0) ? 1 : 0;
}
