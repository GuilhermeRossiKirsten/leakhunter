#include "leakhunter/registry/AllocationRegistry.hpp"

#include <algorithm>
#include <utility>

namespace leakhunter::registry {

void AllocationRegistry::setSessionOrigin(std::uint64_t pid, std::uint64_t startTimestampNs) {
    stats_.pid = pid;
    stats_.startTimestampNs = startTimestampNs;
}

void AllocationRegistry::recordAllocation(AllocationInfo&& allocation) {
    ++stats_.totalAllocations;
    stats_.totalBytesAllocated += allocation.size;

    // A zero address means the allocator itself failed; count it, but there is
    // nothing to track and nothing that can leak.
    if (allocation.address == 0) {
        return;
    }

    if (allocation.kind == AllocationKind::New || allocation.kind == AllocationKind::NewArray) {
        ++stats_.newAllocations;
    }

    const std::uint64_t address = allocation.address;
    const std::uint64_t size = allocation.size;

    // An address reappearing while still live means we missed its free (the
    // allocator reused the block). Trust the newer record and drop the stale
    // one, otherwise the old stack trace would be blamed for a live block.
    if (const auto existing = live_.find(address); existing != live_.end()) {
        liveBytes_ -= std::min(liveBytes_, existing->second.size);
        existing->second = std::move(allocation);
    } else {
        live_.emplace(address, std::move(allocation));
    }

    liveBytes_ += size;
    stats_.peakLiveBytes = std::max(stats_.peakLiveBytes, liveBytes_);
}

bool AllocationRegistry::recordDeallocation(std::uint64_t address, std::uint64_t timestampNs,
                                            ReleaseKind releasedBy, std::uint64_t threadId) {
    ++stats_.totalDeallocations;

    if (releasedBy == ReleaseKind::Delete || releasedBy == ReleaseKind::DeleteArray) {
        ++stats_.deleteReleases;
    }

    if (address == 0) {
        return true;  // free(nullptr) is a no-op, not a mismatch
    }

    const auto it = live_.find(address);
    if (it == live_.end()) {
        // Allocated before the agent was live (e.g. by the dynamic loader), or
        // freed twice. Neither is a leak, so it is counted and ignored.
        ++stats_.untrackedFrees;
        return false;
    }

    const AllocationInfo& allocation = it->second;

    if (!isCompatibleRelease(allocation.kind, releasedBy)) {
        ++stats_.mismatchedFrees;

        // Copying the call stack is the only allocation this path adds, and it
        // happens once per bug rather than once per free.
        if (mismatches_.size() < kMaxRecordedMismatches) {
            MismatchedFree mismatch;
            mismatch.address = address;
            mismatch.size = allocation.size;
            mismatch.timestampNs = timestampNs;
            mismatch.allocatedOnThread = allocation.threadId;
            mismatch.releasedOnThread = threadId;
            mismatch.allocatedBy = allocation.kind;
            mismatch.releasedBy = releasedBy;
            mismatch.callStack = allocation.callStack;
            mismatches_.push_back(std::move(mismatch));
        }
    }

    stats_.totalBytesFreed += allocation.size;
    liveBytes_ -= std::min(liveBytes_, allocation.size);
    live_.erase(it);
    return true;
}

bool AllocationRegistry::mismatchDetectionIsTrustworthy() const noexcept {
    // Both halves of the C++ pair must be interposed, or neither. If exactly
    // one is, every new/free pairing we derive is an artefact of that gap:
    //   * new hooked, delete not  -> every `new` looks freed with free()
    //   * delete hooked, new not  -> every `delete` looks applied to a malloc
    // Requiring agreement costs one false negative -- a program whose only C++
    // allocation is the buggy one -- and removes both false-positive floods.
    return (stats_.newAllocations > 0) == (stats_.deleteReleases > 0);
}

std::vector<MismatchedFree> AllocationRegistry::takeMismatchedFrees() {
    if (!mismatchDetectionIsTrustworthy()) {
        // Our operator delete never ran, so every one of these is an artefact
        // of the interposition, not a bug in the target.
        mismatches_.clear();
        stats_.mismatchedFrees = 0;
        return {};
    }
    return std::move(mismatches_);
}

std::vector<AllocationInfo> AllocationRegistry::takeLiveAllocations() {
    std::vector<AllocationInfo> result;
    result.reserve(live_.size());

    for (auto& [address, allocation] : live_) {
        result.push_back(std::move(allocation));
    }
    live_.clear();
    liveBytes_ = 0;

    return result;
}

void AllocationRegistry::applyAgentSummary(std::uint64_t droppedRecords,
                                           std::uint64_t truncatedTraces,
                                           std::uint64_t endTimestampNs) {
    stats_.droppedRecords = droppedRecords;
    stats_.truncatedTraces = truncatedTraces;
    stats_.endTimestampNs = endTimestampNs;
}

}  // namespace leakhunter::registry
