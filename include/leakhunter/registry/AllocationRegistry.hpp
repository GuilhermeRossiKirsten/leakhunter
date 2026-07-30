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

private:
    std::unordered_map<std::uint64_t, AllocationInfo> live_;
    std::vector<MismatchedFree> mismatches_;
    SessionStats stats_;
    std::uint64_t liveBytes_ = 0;
};

}  // namespace leakhunter::registry
