/// @file LeakReport.hpp
/// @brief The analysed result of a monitored run -- the report generators' input.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "leakhunter/analysis/LeakTriage.hpp"
#include "leakhunter/analysis/MemoryTimeline.hpp"
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

/// Mismatched frees sharing a call site *and* a pairing, which is how someone
/// fixing them wants to read them: "this line does new[]/free(), 8 times".
///
/// Without this the report lists every occurrence separately, and eight turns
/// of one loop look exactly like eight separate bugs -- especially since the
/// allocator hands back the same address each time, so even the addresses match.
struct MismatchGroup {
    std::string function;
    std::string module;
    std::string location;  ///< "file:line" when known

    /// Part of the key, not just decoration: one function can get the pairing
    /// wrong in two different ways, and they are two different bugs with two
    /// different fixes.
    AllocationKind allocatedBy = AllocationKind::Unknown;
    ReleaseKind releasedBy = ReleaseKind::Unknown;

    std::uint64_t count = 0;
    std::uint64_t totalBytes = 0;

    /// How many distinct addresses the occurrences touched.
    ///
    /// Fewer than `count` means the allocator kept handing back the same block
    /// -- a loop that frees and reallocates. That is exactly the distinction
    /// needed to answer "did this run eight times, or was it reported eight
    /// times?", and the flat listing cannot express it.
    std::uint64_t distinctAddresses = 0;

    std::uint64_t firstSeenNs = 0;
    std::uint64_t lastSeenNs = 0;
    std::uint64_t threadCount = 0;

    StackTrace representativeTrace;
    std::size_t blamedFrame = 0;
    SourceSnippet snippet;

    std::vector<std::size_t> mismatchIndices;  ///< into LeakReport::mismatchedFrees

    /// True when the same block was released wrongly more than once, i.e. the
    /// occurrences are iterations rather than separate sites.
    [[nodiscard]] bool recycledSameBlock() const noexcept {
        return count > 1 && distinctAddresses < count;
    }
};

/// A call site whose memory crosses a thread boundary.
///
/// Not a defect by itself: producer/consumer queues, thread pools and work
/// stealing all hand blocks between threads on purpose. It is reported because
/// it is the **precondition** for the concurrency bugs that are hardest to
/// find -- a use-after-free or a double free across threads needs the block to
/// cross first -- and because it is invisible to everything else in a leak
/// report.
///
/// Where this differs from a race detector: Helgrind and ThreadSanitizer report
/// a race only if it actually fires during the run they watch. This reports the
/// *shape* on every run, including the runs where the timing happened to work
/// out. That makes it useful in CI, where the schedule is never the production
/// schedule.
struct ThreadHandoff {
    std::string function;
    std::string module;
    std::string location;
    StackTrace representativeTrace;
    std::size_t blamedFrame = 0;

    /// Blocks released by a thread other than the one that allocated them.
    std::uint64_t crossThreadFrees = 0;
    /// ...and blocks that stayed on one thread, for proportion.
    std::uint64_t sameThreadFrees = 0;

    std::uint64_t allocatingThreadCount = 0;
    std::uint64_t releasingThreadCount = 0;

    /// What this pattern means and what to check. Written for someone who has
    /// to decide whether to act, not for someone who already knows.
    std::vector<std::string> advice;

    /// Every release crossed a thread boundary, which is what a deliberate
    /// handoff looks like. A mixture is more often accidental.
    [[nodiscard]] bool alwaysCrosses() const noexcept {
        return crossThreadFrees > 0 && sameThreadFrees == 0;
    }
};

/// A call site ranked by how much memory passed through it, leaked or not.
///
/// Deliberately not a leak. A function that allocates 4 GiB across a run and
/// releases every byte is invisible to a leak report and is often the most
/// expensive thing in the profile -- and the fix (reserve, pool, reuse) has
/// nothing to do with ownership.
struct HotSpot {
    std::string function;
    std::string module;
    std::string location;  ///< "file:line" when known
    StackTrace representativeTrace;
    std::size_t blamedFrame = 0;

    std::uint64_t totalBytes = 0;     ///< ever allocated here
    std::uint64_t count = 0;          ///< allocations, including freed ones
    std::uint64_t peakLiveBytes = 0;  ///< most this site held at one moment

    /// Still outstanding at exit and counted against the program, matching
    /// what `leakedBytes` means in the summary.
    std::uint64_t liveBytes = 0;
    std::uint64_t liveCount = 0;

    /// Still outstanding, but requested from inside libc or the loader --
    /// stdio buffers, locale tables. Split out for the same reason the summary
    /// splits `runtimeLeakedBytes` from `leakedBytes`.
    ///
    /// The split has to happen here rather than being inherited, because the
    /// two views disagree by design: `classifyOrigin` looks at the *first*
    /// non-allocator frame and sees `libc`, while `findResponsibleFrame` walks
    /// out of the runtime to give the reader something actionable and sees
    /// `main`. Without this, a stdio buffer shows up as bytes `main` is still
    /// holding, in a report whose verdict is PASSED.
    std::uint64_t runtimeLiveBytes = 0;
    std::uint64_t runtimeLiveCount = 0;

    /// Bytes allocated per byte ever held at once, for this site alone.
    /// High means the site churns; the memory is going back, repeatedly.
    [[nodiscard]] double turnover() const noexcept {
        return peakLiveBytes == 0 ? 0.0
                                  : static_cast<double>(totalBytes) /
                                        static_cast<double>(peakLiveBytes);
    }

    [[nodiscard]] std::uint64_t averageBytes() const noexcept {
        return count == 0 ? 0 : totalBytes / count;
    }
};

struct LeakReport {
    SessionStats stats;
    ProcessResult process;

    /// How much memory the target held as it ran, rather than what it was left
    /// holding at the end. Empty when the timeline was not collected.
    MemoryTimeline timeline;

    /// Where the memory went, leaked or not. Sorted by totalBytes, descending.
    std::vector<HotSpot> hotSpots;

    /// Sites whose blocks cross a thread boundary. Sorted by crossThreadFrees,
    /// descending. Built from every tracked site, not just the hot ones -- a
    /// handoff matters at any volume.
    std::vector<ThreadHandoff> threadHandoffs;

    /// True when more distinct call sites existed than could be tracked, so
    /// the ranking is biased towards sites that appeared early in the run.
    bool hotSpotsTruncated = false;

    std::vector<Leak> leaks;        ///< sorted by size, descending
    std::vector<LeakGroup> groups;  ///< sorted by totalBytes, descending

    /// Blocks released through the wrong entry point. Not leaks -- the memory
    /// was returned -- but undefined behaviour, and reported alongside because
    /// the tool is already holding the evidence.
    std::vector<MismatchedFree> mismatchedFrees;

    /// The same findings by call site and pairing. Sorted by count, descending.
    ///
    /// Kept alongside the flat list rather than replacing it, exactly as
    /// `groups` sits alongside `leaks`: consumers that walk every occurrence
    /// keep working.
    std::vector<MismatchGroup> mismatchGroups;

    /// True when more mismatches happened than were listed, so the group counts
    /// describe a sample rather than the total.
    bool mismatchGroupsArePartial = false;

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
