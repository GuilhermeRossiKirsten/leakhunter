/// @file LeakReport.hpp
/// @brief The analysed result of a monitored run -- the report generators' input.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "leakhunter/analysis/LeakTriage.hpp"
#include "leakhunter/core/Types.hpp"

namespace leakhunter::analysis {

/// Where an allocation was requested from.
///
/// Every block still live at exit is technically a leak, but not every one is
/// actionable: the C runtime allocates buffers it deliberately never frees
/// (stdio buffers, locale tables, loader bookkeeping). Reporting those next to
/// real application leaks is how a leak detector trains people to ignore it.
enum class LeakOrigin : std::uint8_t {
    Application = 0,  ///< requested by code outside the C runtime
    Runtime = 1,      ///< requested from inside libc / ld.so
};

/// Whether the mismatched-free check actually ran.
///
/// "Zero mismatched frees" means two very different things depending on this,
/// and a report that does not say which is lying by omission.
enum class MismatchCheck : std::uint8_t {
    Active = 0,      ///< the check ran; zero findings means zero mismatches
    Suppressed = 1,  ///< one-sided interposition; the check could not be trusted
    Disabled = 2,    ///< --no-mismatch-check
};

[[nodiscard]] std::string_view toString(MismatchCheck state) noexcept;

/// One leaked block.
struct Leak {
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t timestampNs = 0;   ///< relative to process start
    std::uint64_t threadId = 0;
    AllocationKind kind = AllocationKind::Unknown;
    LeakOrigin origin = LeakOrigin::Application;
    StackTrace trace;

    /// Index into `trace` of the frame blamed for the leak, i.e. the first
    /// frame that belongs to user code rather than to an allocator.
    std::size_t responsibleFrame = 0;

    [[nodiscard]] const StackFrame* responsible() const noexcept {
        return responsibleFrame < trace.size() ? &trace[responsibleFrame] : nullptr;
    }
};

/// Leaks sharing the same responsible function, which is how a developer wants
/// to read them: "this function leaked 4 MiB across 1024 allocations".
struct LeakGroup {
    std::string function;
    std::string module;
    std::string location;     ///< "file:line" when known
    std::uint64_t totalBytes = 0;
    std::uint64_t count = 0;
    std::uint64_t threadCount = 0;
    StackTrace representativeTrace;

    /// Index into `representativeTrace` of the blamed frame, so the report can
    /// highlight it without re-deriving the attribution.
    std::size_t blamedFrame = 0;

    /// The blamed line in its source context. One per site, not per leak: 700
    /// leaks across 3 sites means 3 snippets.
    SourceSnippet snippet;

    /// How bad this site is and what to do about it. Filled in by
    /// triageLeaks() after grouping; empty when nothing could be determined.
    LeakTriage triage;

    std::vector<std::size_t> leakIndices;  ///< into LeakReport::leaks
};

struct LeakReport {
    SessionStats stats;
    ProcessResult process;

    std::vector<Leak> leaks;        ///< sorted by size, descending
    std::vector<LeakGroup> groups;  ///< sorted by totalBytes, descending

    /// Blocks released through the wrong entry point. Not leaks -- the memory
    /// was returned -- but undefined behaviour, and reported alongside because
    /// the tool is already holding the evidence.
    std::vector<MismatchedFree> mismatchedFrees;

    /// Mismatches counted but not listed (beyond the listing cap).
    std::uint64_t suppressedMismatches = 0;

    /// Mismatched frees matched by a --suppressions rule. Like suppressed leaks,
    /// these leave the headline count entirely, so a vendored library's
    /// unfixable UB can be accepted rather than blocking every build.
    std::uint64_t suppressedMismatchesByRules = 0;

    /// Set by the application once it knows whether the check could run.
    MismatchCheck mismatchCheck = MismatchCheck::Active;

    /// Headline numbers: application leaks only, unless --include-runtime was
    /// given, in which case runtime allocations are folded in here too.
    std::uint64_t leakedBytes = 0;
    std::uint64_t leakCount = 0;

    /// Runtime allocations still live at exit. Always counted, listed only on
    /// request. Non-zero here is normal and not a defect.
    std::uint64_t runtimeLeakCount = 0;
    std::uint64_t runtimeLeakedBytes = 0;

    /// Leaks filtered out by --min-leak-size (still counted in the totals).
    std::uint64_t suppressedLeaks = 0;
    std::uint64_t suppressedBytes = 0;

    /// Leaks matched by a --suppressions rule. Unlike the two above, these are
    /// excluded from `leakCount`/`leakedBytes` entirely: the point of a
    /// suppression is that the leak stops counting against you. They are
    /// reported here, and in the per-rule hit counts, so the exclusion is never
    /// invisible.
    std::uint64_t suppressedByRules = 0;
    std::uint64_t suppressedByRulesBytes = 0;

    /// One entry per rule that fired, for the report. Kept as plain data rather
    /// than a pointer into the SuppressionSet so the report stays self-contained.
    struct RuleHit {
        std::string rule;         ///< "leaks.supp:12: stack:*/vendor/*"
        std::uint64_t count = 0;  ///< findings suppressed: leaks + mismatched frees
        /// Leaked bytes only. A suppressed mismatched free contributes to
        /// `count` but not here: that block was returned, so counting its size
        /// as suppressed memory would overstate the impact.
        std::uint64_t bytes = 0;
    };
    std::vector<RuleHit> ruleHits;

    /// Rules that matched nothing, rendered for diagnostics.
    std::vector<std::string> unusedRules;

    std::string targetCommand;
    std::string generatedAtIso8601;
    std::string toolVersion;

    /// No leaks and no undefined behaviour: the run passes. This is what drives
    /// the process exit code, so a mismatched free fails a CI gate exactly like
    /// a leak does.
    [[nodiscard]] bool clean() const noexcept {
        return leakCount == 0 && stats.mismatchedFrees == 0;
    }
};

}  // namespace leakhunter::analysis
