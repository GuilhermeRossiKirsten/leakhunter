/// @file AllocationRegistry.hpp
/// @brief Hash-map backed live-allocation registry.

#pragma once

#include <unordered_map>
#include <vector>

#include "leakhunter/registry/IAllocationRegistry.hpp"

namespace leakhunter::registry {

/// Replays the allocate/free stream and keeps whatever is still outstanding.
///
/// This runs in the host process, not the target, so it is free to use the
/// standard library. It is intentionally single-threaded: the trace is replayed
/// sequentially and the recorded thread ids preserve the concurrency
/// information we care about.
class AllocationRegistry final : public IAllocationRegistry {
public:
    AllocationRegistry() = default;

    /// Upper bound on individually recorded mismatches. A program that gets the
    /// pairing wrong usually gets it wrong in a loop; past a few thousand the
    /// list stops informing and starts bloating the report. The count is exact
    /// regardless.
    static constexpr std::size_t kMaxRecordedMismatches = 2000;

    void recordAllocation(AllocationInfo&& allocation) override;
    bool recordDeallocation(std::uint64_t address, std::uint64_t timestampNs,
                            ReleaseKind releasedBy, std::uint64_t threadId) override;

    [[nodiscard]] std::vector<AllocationInfo> takeLiveAllocations() override;
    [[nodiscard]] std::vector<MismatchedFree> takeMismatchedFrees() override;

    [[nodiscard]] const SessionStats& stats() const noexcept override { return stats_; }
    [[nodiscard]] std::size_t liveCount() const noexcept override { return live_.size(); }

    /// False when the evidence says our `operator delete` interposition never
    /// took effect, which makes every new/free pairing meaningless.
    ///
    /// This happens for real: the dynamic linker searches the executable itself
    /// before any LD_PRELOAD object, so a program that links a static
    /// libstdc++, or defines its own global `operator delete`, keeps its own
    /// definition. If that definition then calls `free()`, every `new` in the
    /// program looks like it was released with `free()`. Seeing `new` used but
    /// `delete` never observed is exactly that situation, and the whole class
    /// of findings is suppressed rather than reported as thousands of bugs.
    [[nodiscard]] bool mismatchDetectionIsTrustworthy() const noexcept;

    /// Merges the agent-side counters, which are authoritative for anything
    /// that happened before the host started reading (and for dropped records).
    void applyAgentSummary(std::uint64_t droppedRecords, std::uint64_t truncatedTraces,
                           std::uint64_t endTimestampNs);

    void setSessionOrigin(std::uint64_t pid, std::uint64_t startTimestampNs);

    /// Upper bound on retained timeline events, at 16 bytes each: 32 MiB.
    ///
    /// Every allocation and every free contributes one, so this is roughly a
    /// million allocations of headroom -- past cmake's 178k, short of a long
    /// server run. Beyond it the timeline is truncated rather than allowed to
    /// grow without limit, and `timelineTruncated()` says so.
    static constexpr std::size_t kMaxTimelineEvents = 2'000'000;

    /// Starts recording the shape of memory use over time.
    ///
    /// Off by default: the events cost memory proportional to the trace, and
    /// nothing but the report needs them.
    void enableTimeline() noexcept { collectTimeline_ = true; }

    [[nodiscard]] bool timelineTruncated() const noexcept { return timelineTruncated_; }

    [[nodiscard]] std::vector<MemoryEvent> takeTimelineEvents() { return std::move(timeline_); }

    /// Upper bound on distinct call sites tracked for allocation volume.
    ///
    /// Bounded by how many distinct stacks a program has, not by how many
    /// allocations it makes, so this is generous: a large C++ program has a few
    /// thousand. Past it, existing sites keep accumulating and new ones are
    /// dropped, which biases towards the sites that appeared early.
    static constexpr std::size_t kMaxTrackedSites = 20'000;

    /// Starts accumulating per-site allocation volume, including blocks that
    /// are later freed. Off by default, for the same reason as the timeline.
    void enableSiteTracking() noexcept { collectSites_ = true; }

    [[nodiscard]] bool siteTrackingTruncated() const noexcept { return sitesTruncated_; }

    /// Every tracked site. Unordered -- the analyzer sorts once it knows the
    /// names, so there is nothing gained by ordering them twice.
    [[nodiscard]] std::vector<AllocationSite> takeAllocationSites();

private:
    /// Appends a delta if the timeline is on and has room.
    void noteTimeline(std::uint64_t timestampNs, std::int64_t deltaBytes);

    /// Charges @p size to the site @p callStack identifies, or discharges it
    /// when @p size is negative.
    void chargeSite(const std::vector<std::uint64_t>& callStack, std::int64_t size);

    std::unordered_map<std::uint64_t, AllocationInfo> live_;
    std::vector<MismatchedFree> mismatches_;
    SessionStats stats_;
    std::uint64_t liveBytes_ = 0;

    std::vector<MemoryEvent> timeline_;
    bool collectTimeline_ = false;
    bool timelineTruncated_ = false;

    /// Keyed by a hash of the call stack. Collisions merge two sites into one,
    /// which costs accuracy in the *ordering* of a report section and never
    /// affects a leak verdict -- worth it to avoid keying on the stack itself.
    std::unordered_map<std::uint64_t, AllocationSite> sites_;
    bool collectSites_ = false;
    bool sitesTruncated_ = false;
};

}  // namespace leakhunter::registry
