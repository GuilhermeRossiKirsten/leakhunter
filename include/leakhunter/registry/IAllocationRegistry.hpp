/// @file IAllocationRegistry.hpp
/// @brief Book-keeping of live allocations.

#pragma once

#include <cstdint>
#include <vector>

#include "leakhunter/core/Types.hpp"

namespace leakhunter::registry {

class IAllocationRegistry {
public:
    virtual ~IAllocationRegistry() = default;

    virtual void recordAllocation(AllocationInfo&& allocation) = 0;

    /// @param releasedBy Entry point that freed the block, so a release through
    ///        the wrong one can be paired with how it was allocated.
    /// @return true when @p address matched a live allocation.
    virtual bool recordDeallocation(std::uint64_t address, std::uint64_t timestampNs,
                                    ReleaseKind releasedBy, std::uint64_t threadId) = 0;

    /// Allocations still outstanding -- the leak candidates.
    [[nodiscard]] virtual std::vector<AllocationInfo> takeLiveAllocations() = 0;

    /// Blocks released through an incompatible entry point, in the order they
    /// were released. Capped; the true total lives in stats().mismatchedFrees.
    [[nodiscard]] virtual std::vector<MismatchedFree> takeMismatchedFrees() = 0;

    [[nodiscard]] virtual const SessionStats& stats() const noexcept = 0;
    [[nodiscard]] virtual std::size_t liveCount() const noexcept = 0;
};

}  // namespace leakhunter::registry
