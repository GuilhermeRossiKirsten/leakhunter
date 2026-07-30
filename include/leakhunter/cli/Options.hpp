/// @file Options.hpp
/// @brief Fully-resolved configuration for one LeakHunter run.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "leakhunter/core/Logger.hpp"

namespace leakhunter::cli {

enum class ReportFormat : std::uint8_t {
    Html = 1 << 0,
    Json = 1 << 1,
};

struct Options {
    /// argv of the monitored program: [0] is the executable, rest are its args.
    std::vector<std::string> targetCommand;

    std::filesystem::path outputDirectory{"leakhunter-report"};
    bool emitHtml = true;
    bool emitJson = true;

    /// Frames captured per allocation. Lower is faster, higher is more context.
    std::uint16_t maxFrames = 32;

    /// Leaks smaller than this are counted in the totals but not listed.
    std::uint64_t minLeakSize = 0;

    /// List blocks the C runtime never frees (stdio buffers, locale tables)
    /// alongside application leaks. Off by default: they are expected.
    bool includeRuntimeLeaks = false;

    /// Resolve file:line with llvm-symbolizer when it is on PATH.
    bool resolveSourceLocations = true;

    /// Read the source files and embed the blamed lines in the reports.
    ///
    /// On by default, which has one consequence worth stating: report.html then
    /// contains excerpts of your source, so sharing the artifact shares code.
    /// `--no-source-snippets` turns it off; `sourceRoots` can confine it.
    bool sourceSnippets = true;

    /// Lines of context on each side of the blamed line.
    std::uint32_t snippetContext = 4;

    /// Directories to search when a recorded source path does not exist here --
    /// a report generated somewhere other than where the target was built.
    std::vector<std::filesystem::path> sourceRoots;

    /// Also write compiler-style findings to stderr, for editors and CI.
    bool emitDiagnostics = false;

    /// Report blocks released through the wrong entry point (`new[]` freed with
    /// `delete`, and so on). On by default; the escape hatch exists because the
    /// check depends on both halves of the C++ pair being interposed, which a
    /// program with its own global `operator new`/`delete` can defeat.
    bool detectMismatchedFrees = true;

    /// Files of rules for leaks that are known and accepted. Repeatable, and
    /// applied in the order given.
    std::vector<std::filesystem::path> suppressionFiles;

    /// Fail the run when a suppression rule matched nothing. Off by default —
    /// a rotted rule is a warning, and only a project that has decided to keep
    /// its suppression file honest wants it to be fatal.
    bool strictSuppressions = false;

    /// Keep the intermediate binary trace instead of deleting it.
    bool keepTrace = false;

    /// Explicit trace path; empty means "use a temporary file".
    std::filesystem::path traceFile;

    /// Explicit agent library path; empty means "discover it".
    std::filesystem::path agentLibrary;

    log::Level verbosity = log::Level::Normal;

    /// Set when the parser handled everything itself (--help, --version).
    bool shouldExitEarly = false;
    int earlyExitCode = 0;
};

}  // namespace leakhunter::cli
