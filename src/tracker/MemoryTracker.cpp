#include "leakhunter/tracker/MemoryTracker.hpp"

#include <utility>

#include "leakhunter/core/Logger.hpp"

namespace leakhunter::tracker {

MemoryTracker::MemoryTracker(registry::AllocationRegistry& registry,
                             symbols::SymbolResolver& resolver)
    : registry_(registry), resolver_(resolver) {}

Status MemoryTracker::consume(ITraceSource& source) {
    return source.replay(*this);
}

void MemoryTracker::onTraceBegin(const TraceInfo& info) {
    info_ = info;
    registry_.setSessionOrigin(info.pid, info.startTimestampNs);
    log::debug("trace begin: pid={} format=v{} maxFrames={}", info.pid, info.formatVersion,
               info.maxFrames);
}

void MemoryTracker::onAllocation(AllocationInfo&& allocation) {
    // Register the call sites before the allocation moves on. On a healthy run
    // the agent's dladdr pass has already described them and this is a hash
    // hit; on a crashed run it is the only thing that places these addresses
    // inside a module.
    for (const std::uint64_t programCounter : allocation.callStack) {
        resolver_.observe(programCounter);
    }

    registry_.recordAllocation(std::move(allocation));
}

void MemoryTracker::onModule(ModuleRange&& module) {
    resolver_.addModule(std::move(module));
}

void MemoryTracker::onDeallocation(std::uint64_t address, std::uint64_t timestampNs,
                                   ReleaseKind releasedBy, std::uint64_t threadId) {
    registry_.recordDeallocation(address, timestampNs, releasedBy, threadId);
}

void MemoryTracker::onSymbol(RawSymbol&& symbol) {
    resolver_.addSymbol(std::move(symbol));
}

void MemoryTracker::onTraceEnd(const TraceSummary& summary) {
    sawEnd_ = summary.endTimestampNs != 0;
    registry_.applyAgentSummary(summary.droppedRecords, summary.truncatedTraces,
                                summary.endTimestampNs);

    log::debug("trace end: {} allocations, {} frees, {} symbols, {} live",
               summary.totalAllocations, summary.totalDeallocations, resolver_.size(),
               registry_.liveCount());

    if (summary.droppedRecords > 0 && sawEnd_) {
        log::warn("{} records were dropped by the agent -- increase the trace buffer or reduce "
                  "--max-frames; the report may under-count leaks",
                  summary.droppedRecords);
    }
    if (summary.truncatedTraces > 0) {
        log::debug("{} stack traces hit the --max-frames limit", summary.truncatedTraces);
    }
}

}  // namespace leakhunter::tracker
