#include "leakhunter/source/SourceSnippetReader.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

#include "leakhunter/core/Logger.hpp"

namespace leakhunter::source {
namespace fs = std::filesystem;
namespace {

/// Expands tabs and drops a trailing CR.
///
/// Tabs are expanded here rather than in the renderers because the HTML, the
/// terminal and a JSON consumer would otherwise each need their own tab policy,
/// and the column numbers the symbolizer reports would agree with none of them.
[[nodiscard]] std::string normalise(std::string_view raw, std::uint32_t tabWidth) {
    if (!raw.empty() && raw.back() == '\r') {
        raw.remove_suffix(1);
    }

    if (raw.find('\t') == std::string_view::npos) {
        return std::string{raw};
    }

    std::string expanded;
    expanded.reserve(raw.size() + 8);
    for (const char character : raw) {
        if (character != '\t') {
            expanded.push_back(character);
            continue;
        }
        const std::size_t width = tabWidth == 0 ? 1 : tabWidth;
        const std::size_t pad = width - (expanded.size() % width);
        expanded.append(pad, ' ');
    }
    return expanded;
}

/// Splits @p path into its components, so suffixes of it can be rebuilt.
[[nodiscard]] std::vector<std::string> components(const fs::path& path) {
    std::vector<std::string> parts;
    for (const fs::path& part : path) {
        const std::string text = part.string();
        // Skip the root ("/" and, on Windows, "C:") -- a suffix never includes it.
        if (text.empty() || text == "/" || text == "\\" || part.has_root_name()) {
            continue;
        }
        parts.push_back(text);
    }
    return parts;
}

}  // namespace

SourceSnippetReader::SourceSnippetReader(SnippetConfig config) : config_(std::move(config)) {}

fs::path SourceSnippetReader::locate(const std::string& recorded) const {
    if (recorded.empty()) {
        return {};
    }

    std::error_code ec;
    const fs::path asRecorded{recorded};
    if (fs::is_regular_file(asRecorded, ec)) {
        return asRecorded;
    }

    // Try each root against progressively shorter suffixes of the path, longest
    // first: `/build/agent/1/src/app/foo.cpp` under root `/home/me/proj` finds
    // `/home/me/proj/src/app/foo.cpp` without anyone having to say how many
    // leading components to strip.
    const std::vector<std::string> parts = components(asRecorded);
    for (const fs::path& root : config_.roots) {
        for (std::size_t skip = 0; skip < parts.size(); ++skip) {
            fs::path candidate = root;
            for (std::size_t i = skip; i < parts.size(); ++i) {
                candidate /= parts[i];
            }
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }

    return {};
}

const std::vector<std::string>* SourceSnippetReader::lines(const std::string& file) {
    if (const auto cached = cache_.find(file); cached != cache_.end()) {
        return cached->second.empty() ? nullptr : &cached->second;
    }

    // Cache the failure too, so a file that is missing does not get looked up
    // once per leak site.
    std::vector<std::string>& slot = cache_[file];

    const fs::path resolved = locate(file);
    if (resolved.empty()) {
        missingFiles_.push_back(file);
        return nullptr;
    }

    std::error_code ec;

    // is_regular_file, checked again on the resolved path and before opening:
    // the recorded path comes from the traced binary's debug info, and reading
    // a FIFO or a character device would block forever rather than fail.
    if (!fs::is_regular_file(resolved, ec)) {
        return nullptr;
    }

    const std::uintmax_t size = fs::file_size(resolved, ec);
    if (ec || size > config_.maxFileBytes) {
        log::debug("skipping source '{}': {}", resolved.string(),
                   ec ? "cannot size it" : "larger than the snippet limit");
        return nullptr;
    }

    std::ifstream input(resolved, std::ios::binary);
    if (!input) {
        return nullptr;
    }

    std::string text;
    while (std::getline(input, text)) {
        slot.push_back(normalise(text, config_.tabWidth));
    }

    if (slot.empty()) {
        return nullptr;
    }
    return &slot;
}

SourceSnippet SourceSnippetReader::read(const std::string& file, std::uint32_t line,
                                        std::uint32_t column) {
    SourceSnippet snippet;
    if (file.empty() || line == 0 || budgetExhausted_) {
        return snippet;
    }

    const std::vector<std::string>* content = lines(file);
    if (content == nullptr) {
        return snippet;
    }

    const auto lineCount = static_cast<std::uint32_t>(content->size());
    if (line > lineCount) {
        // Debug info that disagrees with the file on disk -- a stale build, or a
        // source that has been edited since. Better to show nothing than to
        // highlight an unrelated line with confidence.
        log::debug("source '{}' has {} lines but the report blames line {}", file, lineCount, line);
        return snippet;
    }

    const std::uint32_t first = line > config_.contextLines ? line - config_.contextLines : 1;
    const std::uint32_t last = std::min(lineCount, line + config_.contextLines);

    std::size_t bytes = 0;
    for (std::uint32_t current = first; current <= last; ++current) {
        bytes += (*content)[current - 1].size() + 1;
    }

    if (emittedBytes_ + bytes > config_.maxTotalBytes) {
        budgetExhausted_ = true;
        log::debug("snippet budget of {} bytes reached; later sites will have no source",
                   config_.maxTotalBytes);
        return snippet;
    }
    emittedBytes_ += bytes;

    snippet.file = file;
    snippet.firstLine = first;
    snippet.blamedLine = line;
    snippet.column = column;
    snippet.lines.reserve(last - first + 1);
    for (std::uint32_t current = first; current <= last; ++current) {
        snippet.lines.push_back((*content)[current - 1]);
    }
    return snippet;
}

std::size_t SourceSnippetReader::enrich(analysis::LeakReport& report) {
    std::size_t produced = 0;

    for (analysis::LeakGroup& group : report.groups) {
        const StackFrame* frame = group.blamedFrame < group.representativeTrace.size()
                                      ? &group.representativeTrace[group.blamedFrame]
                                      : nullptr;
        if (frame == nullptr) {
            continue;
        }
        group.snippet = read(frame->file, frame->line, frame->column);
        produced += group.snippet.empty() ? 0 : 1;
    }

    for (MismatchedFree& mismatch : report.mismatchedFrees) {
        const StackFrame* frame = mismatch.responsible();
        if (frame == nullptr) {
            continue;
        }
        mismatch.snippet = read(frame->file, frame->line, frame->column);
        produced += mismatch.snippet.empty() ? 0 : 1;
    }

    // Groups borrow the snippet their first occurrence already produced, rather
    // than reading it again. Same file, same line, same bytes -- re-reading
    // would spend the per-report byte budget on a duplicate and could push a
    // later, genuinely different site past the limit.
    for (analysis::MismatchGroup& group : report.mismatchGroups) {
        if (group.mismatchIndices.empty()) {
            continue;
        }
        const std::size_t first = group.mismatchIndices.front();
        if (first < report.mismatchedFrees.size()) {
            group.snippet = report.mismatchedFrees[first].snippet;
        }
    }

    // De-duplicate before anyone reports it: the same missing file is looked up
    // once per site.
    std::sort(missingFiles_.begin(), missingFiles_.end());
    missingFiles_.erase(std::unique(missingFiles_.begin(), missingFiles_.end()),
                        missingFiles_.end());

    log::debug("attached {} source snippet(s); {} file(s) not found", produced,
               missingFiles_.size());
    return produced;
}

}  // namespace leakhunter::source
