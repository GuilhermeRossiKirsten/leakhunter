#include "leakhunter/report/DiagnosticsWriter.hpp"

#include <cstdlib>
#include <map>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>

namespace leakhunter::report {
namespace {

/// GitHub's workflow-command syntax has no escape for `::` or newlines inside a
/// message, so anything that could terminate the command early is replaced.
/// Demangled C++ names are full of `::`.
[[nodiscard]] std::string sanitiseForWorkflowCommand(std::string text) {
    for (char& character : text) {
        if (character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    std::string::size_type at = 0;
    while ((at = text.find("::", at)) != std::string::npos) {
        text.replace(at, 2, "\xE2\x88\xB7");  // U+2237 PROPORTION, reads as ::
        at += 3;
    }
    return text;
}

[[nodiscard]] std::string location(const StackFrame& frame) {
    return frame.column > 0 ? fmt::format("{}:{}:{}", frame.file, frame.line, frame.column)
                            : fmt::format("{}:{}", frame.file, frame.line);
}

void emit(std::ostream& out, DiagnosticStyle style, const StackFrame& frame,
          const std::string& category, const std::string& message) {
    if (style == DiagnosticStyle::GitHubActions) {
        out << "::warning file=" << frame.file << ",line=" << frame.line;
        if (frame.column > 0) {
            out << ",col=" << frame.column;
        }
        out << ",title=leakhunter " << category << "::" << sanitiseForWorkflowCommand(message)
            << '\n';
        return;
    }

    out << location(frame) << ": warning: " << category << ": " << message << " [leakhunter:"
        << category << "]\n";
}

}  // namespace

DiagnosticStyle detectDiagnosticStyle() {
    const char* inActions = std::getenv("GITHUB_ACTIONS");
    return inActions != nullptr && *inActions != '\0' ? DiagnosticStyle::GitHubActions
                                                      : DiagnosticStyle::Gcc;
}

void writeDiagnostics(const analysis::LeakReport& report, std::ostream& out,
                      DiagnosticStyle style) {
    std::size_t withoutLocation = 0;

    for (const analysis::LeakGroup& group : report.groups) {
        const StackFrame* frame = group.blamedFrame < group.representativeTrace.size()
                                      ? &group.representativeTrace[group.blamedFrame]
                                      : nullptr;
        if (frame == nullptr || frame->file.empty() || frame->line == 0) {
            ++withoutLocation;
            continue;
        }

        emit(out, style, *frame, "leak",
             fmt::format("{} block(s) leaked here, {} in total, by {}", group.count,
                         formatBytes(group.totalBytes), group.function));
    }

    // Mismatched frees are listed per block, and a site that gets the pairing
    // wrong usually gets it wrong repeatedly -- eight identical annotations on
    // one line of a pull request is noise, not information. Collapse them the
    // way a compiler would: one diagnostic per site, carrying the count.
    struct Site {
        const StackFrame* frame;
        const MismatchedFree* first;
        std::uint64_t count;
        std::uint64_t bytes;
    };
    std::vector<Site> sites;
    std::map<std::tuple<std::string, std::uint32_t, std::uint32_t>, std::size_t> byLocation;

    for (const MismatchedFree& mismatch : report.mismatchedFrees) {
        const StackFrame* frame = mismatch.responsible();
        if (frame == nullptr || frame->file.empty() || frame->line == 0) {
            ++withoutLocation;
            continue;
        }

        const auto key = std::make_tuple(frame->file, frame->line, frame->column);
        const auto [position, inserted] = byLocation.try_emplace(key, sites.size());
        if (inserted) {
            sites.push_back({frame, &mismatch, 0, 0});
        }
        Site& site = sites[position->second];
        ++site.count;
        site.bytes += mismatch.size;
    }

    for (const Site& site : sites) {
        emit(out, style, *site.frame, "mismatched-free",
             fmt::format("{} block(s) allocated with {} here, released with {} -- "
                         "undefined behaviour ({} affected)",
                         site.count, toSourceSpelling(site.first->allocatedBy),
                         toSourceSpelling(site.first->releasedBy), formatBytes(site.bytes)));
    }

    if (withoutLocation > 0) {
        // Say it once. Silently dropping findings would make the diagnostics
        // stream disagree with the report next to it.
        const std::string note = fmt::format(
            "{} finding(s) had no source location and are only in the report "
            "(build with -g, and keep llvm-symbolizer on PATH)",
            withoutLocation);
        if (style == DiagnosticStyle::GitHubActions) {
            out << "::notice title=leakhunter::" << sanitiseForWorkflowCommand(note) << '\n';
        } else {
            out << "leakhunter: note: " << note << '\n';
        }
    }
}

}  // namespace leakhunter::report
