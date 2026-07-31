#include "leakhunter/registry/AllocationRegistry.hpp"

#include <algorithm>
#include <utility>

namespace leakhunter::registry {

void AllocationRegistry::noteTimeline(std::uint64_t timestampNs, std::int64_t deltaBytes) {
    if (!collectTimeline_) {
        return;
    }
    if (timeline_.size() >= kMaxTimelineEvents) {
        timelineTruncated_ = true;
        return;
    }
    timeline_.push_back({timestampNs, deltaBytes});
}

namespace {

/// FNV-1a over the program counters.
///
/// Not cryptographic and does not need to be: a collision merges two call sites
/// in one report section, it cannot turn a leak into a non-leak.
[[nodiscard]] std::uint64_t hashStack(const std::vector<std::uint64_t>& callStack) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint64_t programCounter : callStack) {
        hash ^= programCounter;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

void AllocationRegistry::chargeSite(const std::vector<std::uint64_t>& callStack,
                                    std::int64_t size) {
    if (!collectSites_ || callStack.empty()) {
        return;
    }

    const std::uint64_t key = hashStack(callStack);
    const auto existing = sites_.find(key);

    if (existing == sites_.end()) {
        // A release for a site we are not tracking: the allocation happened
        // before the cap was hit, or before tracking was on. Dropping it is
        // right -- inventing a site from a free would report a call site that
        // never allocated anything.
        if (size < 0) {
            return;
        }
        if (sites_.size() >= kMaxTrackedSites) {
            sitesTruncated_ = true;
            return;
        }
        AllocationSite site;
        site.callStack = callStack;
        site.totalBytes = static_cast<std::uint64_t>(size);
        site.count = 1;
        site.liveBytes = static_cast<std::uint64_t>(size);
        site.liveCount = 1;
        site.peakLiveBytes = static_cast<std::uint64_t>(size);
        sites_.emplace(key, std::move(site));
        return;
    }

    AllocationSite& site = existing->second;
    if (size >= 0) {
        site.totalBytes += static_cast<std::uint64_t>(size);
        ++site.count;
        site.liveBytes += static_cast<std::uint64_t>(size);
        ++site.liveCount;
        site.peakLiveBytes = std::max(site.peakLiveBytes, site.liveBytes);
    } else {
        const std::uint64_t released = static_cast<std::uint64_t>(-size);
        site.liveBytes -= std::min(site.liveBytes, released);
        if (site.liveCount > 0) {
            --site.liveCount;
        }
    }
}

void AllocationRegistry::noteThreads(const std::vector<std::uint64_t>& callStack,
                                     std::uint64_t allocThread, std::uint64_t freeThread,
                                     bool isRelease) {
    if (!collectSites_ || callStack.empty()) {
        return;
    }
    const auto existing = sites_.find(hashStack(callStack));
    if (existing == sites_.end()) {
        return;  // allocated before tracking began, or past the site cap
    }
    AllocationSite& site = existing->second;

    // A tiny linear scan beats a set here: the vector holds at most 16 entries
    // and is almost always 1, so the hashing would cost more than the search.
    const auto remember = [](std::vector<std::uint64_t>& seen, std::uint64_t thread) {
        if (seen.size() >= kMaxThreadsPerSite) {
            return;
        }
        if (std::find(seen.begin(), seen.end(), thread) == seen.end()) {
            seen.push_back(thread);
        }
    };

    remember(site.allocatingThreads, allocThread);
    if (!isRelease) {
        return;
    }

    remember(site.releasingThreads, freeThread);
    if (freeThread != allocThread) {
        ++site.crossThreadFrees;
    } else {
        ++site.sameThreadFrees;
    }
}

std::vector<AllocationSite> AllocationRegistry::takeAllocationSites() {
    std::vector<AllocationSite> result;
    result.reserve(sites_.size());
    for (auto& [key, site] : sites_) {
        result.push_back(std::move(site));
    }
    sites_.clear();
    return result;
}

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
    const std::uint64_t when = allocation.timestampNs;

    // An address reappearing while still live means we missed its free (the
    // allocator reused the block). Trust the newer record and drop the stale
    // one, otherwise the old stack trace would be blamed for a live block.
    const auto existing = live_.find(address);
    if (existing != live_.end()) {
        liveBytes_ -= std::min(liveBytes_, existing->second.size);
        // The free we missed did happen, and both the timeline and the site
        // totals have to see it: a program that recycles addresses would
        // otherwise climb forever while `liveBytes_` -- and reality -- stayed
        // flat.
        noteTimeline(when, -static_cast<std::int64_t>(existing->second.size));
        chargeSite(existing->second.callStack,
                   -static_cast<std::int64_t>(existing->second.size));
    }

    // Charged after the eviction above, not before: doing it the other way
    // round would count both blocks as live at once when they share a site,
    // overstating that site's peak by one allocation.
    chargeSite(allocation.callStack, static_cast<std::int64_t>(size));
    noteThreads(allocation.callStack, allocation.threadId, 0, /*isRelease=*/false);

    if (existing != live_.end()) {
        existing->second = std::move(allocation);
    } else {
        live_.emplace(address, std::move(allocation));
    }

    noteTimeline(when, static_cast<std::int64_t>(size));
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
    noteTimeline(timestampNs, -static_cast<std::int64_t>(allocation.size));
    chargeSite(allocation.callStack, -static_cast<std::int64_t>(allocation.size));
    // The block's own allocating thread, not the site's -- one site can serve
    // several producers, and only the per-block comparison is meaningful.
    noteThreads(allocation.callStack, allocation.threadId, threadId, /*isRelease=*/true);
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
