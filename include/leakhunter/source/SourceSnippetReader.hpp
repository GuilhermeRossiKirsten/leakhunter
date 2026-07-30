/// @file SourceSnippetReader.hpp
/// @brief Reads the source lines around a blamed location.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "leakhunter/analysis/LeakReport.hpp"
#include "leakhunter/core/Types.hpp"

namespace leakhunter::source {

struct SnippetConfig {
    /// Lines of context on each side of the blamed line.
    std::uint32_t contextLines = 4;

    /// Directories to try when the recorded path does not exist here. Each is
    /// matched against progressively shorter suffixes of the recorded path, the
    /// same way `gdb set substitute-path` works -- which is what makes a report
    /// generated on a different machine from the build still show source.
    std::vector<std::filesystem::path> roots;

    /// Refuse files larger than this. Generated sources run to tens of
    /// megabytes and there is nothing readable at line N of them.
    std::size_t maxFileBytes = 4U * 1024U * 1024U;

    /// Ceiling on the source text embedded in one report. A project with
    /// hundreds of leak sites must not turn report.html into a source tarball.
    std::size_t maxTotalBytes = 512U * 1024U;

    /// Tab width used to expand tabs at read time, so rendering does not depend
    /// on anyone's `tab-size`.
    std::uint32_t tabWidth = 4;
};

/// Turns (file, line) into a window of source text.
///
/// Deliberately knows nothing about leaks -- `enrich` is the only place the two
/// ideas meet, and it exists so that LeakAnalyzer can stay free of I/O.
///
/// Files are read at most once each: several leak sites in one file is the
/// common case, not the exception.
class SourceSnippetReader {
public:
    explicit SourceSnippetReader(SnippetConfig config = {});

    /// @return a snippet, or an empty one when the file cannot be read.
    ///         Never an error: absent source is normal.
    [[nodiscard]] SourceSnippet read(const std::string& file, std::uint32_t line,
                                     std::uint32_t column);

    /// Fills in the snippet of every leak group and mismatched free, from the
    /// frame each one blames.
    /// @return how many snippets were produced.
    std::size_t enrich(analysis::LeakReport& report);

    /// Files whose source could not be found. Reported once, so a user with a
    /// remapping problem is told rather than left wondering why the snippets
    /// are missing.
    [[nodiscard]] const std::vector<std::string>& missingFiles() const noexcept {
        return missingFiles_;
    }

    [[nodiscard]] bool budgetExhausted() const noexcept { return budgetExhausted_; }

private:
    /// Loads @p file into the cache, applying every limit. @return nullptr when
    /// it cannot be used.
    [[nodiscard]] const std::vector<std::string>* lines(const std::string& file);

    /// Recorded path -> a path that exists here, or empty.
    [[nodiscard]] std::filesystem::path locate(const std::string& recorded) const;

    SnippetConfig config_;
    std::unordered_map<std::string, std::vector<std::string>> cache_;
    std::vector<std::string> missingFiles_;
    std::size_t emittedBytes_ = 0;
    bool budgetExhausted_ = false;
};

}  // namespace leakhunter::source
