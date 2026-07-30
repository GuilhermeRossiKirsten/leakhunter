/// @file DiagnosticsWriter.hpp
/// @brief Compiler-style findings, for editors and CI.

#pragma once

#include <cstdint>
#include <iosfwd>

#include "leakhunter/analysis/LeakReport.hpp"

namespace leakhunter::report {

enum class DiagnosticStyle : std::uint8_t {
    /// `file:line:col: warning: leak: ... [leakhunter:leak]`
    ///
    /// The format every editor already knows how to parse, so `:cnext` in vim or
    /// the Problems pane in VS Code jumps straight to the allocation.
    Gcc = 0,

    /// `::warning file=...,line=...,col=...::...`
    ///
    /// GitHub Actions workflow commands, which put the finding on the line in
    /// the pull request diff.
    GitHubActions = 1,
};

/// Picks GitHubActions when $GITHUB_ACTIONS is set, Gcc otherwise.
///
/// Auto-detection rather than a second flag: one fewer thing to configure, and
/// in CI the useful choice is never the one you remembered to pass.
[[nodiscard]] DiagnosticStyle detectDiagnosticStyle();

/// Writes one line per leak site and per mismatched free.
///
/// Sites with no source location are skipped rather than emitted with a bogus
/// `line 0`: an editor would jump to the top of a file it cannot open, which is
/// worse than saying nothing. The count of skipped sites is written as a single
/// note so the omission is visible.
void writeDiagnostics(const analysis::LeakReport& report, std::ostream& out,
                      DiagnosticStyle style);

}  // namespace leakhunter::report
