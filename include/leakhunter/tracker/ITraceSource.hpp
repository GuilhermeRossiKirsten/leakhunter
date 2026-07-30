/// @file ITraceSource.hpp
/// @brief Producer/consumer contract for the agent's event stream.

#pragma once

#include <cstdint>
#include <string>

#include "leakhunter/core/Error.hpp"
#include "leakhunter/core/Types.hpp"

namespace leakhunter::tracker {

/// Header information from the start of a trace.
struct TraceInfo {
    std::uint64_t pid = 0;
    std::uint64_t startTimestampNs = 0;
    std::uint32_t formatVersion = 0;
    std::uint16_t maxFrames = 0;
};

/// One loaded object and the address range it covers.
struct ModuleRange {
    std::uint64_t base = 0;
    std::uint64_t span = 0;
    std::string path;
};

/// A dladdr() lookup performed inside the target process.
struct RawSymbol {
    std::uint64_t programCounter = 0;
    std::uint64_t moduleBase = 0;
    std::uint64_t symbolAddress = 0;
    std::string mangledFunction;
    std::string module;
};

/// Agent counters closing the trace.
struct TraceSummary {
    std::uint64_t totalAllocations = 0;
    std::uint64_t totalDeallocations = 0;
    std::uint64_t totalBytesAllocated = 0;
    std::uint64_t totalBytesFreed = 0;
    std::uint64_t droppedRecords = 0;
    std::uint64_t truncatedTraces = 0;
    std::uint64_t endTimestampNs = 0;
};

/// Pull-based visitor invoked once per record, in file order.
class ITraceVisitor {
public:
    virtual ~ITraceVisitor() = default;

    virtual void onTraceBegin(const TraceInfo& info) = 0;
    virtual void onAllocation(AllocationInfo&& allocation) = 0;
    virtual void onDeallocation(std::uint64_t address, std::uint64_t timestampNs,
                                ReleaseKind releasedBy, std::uint64_t threadId) = 0;
    virtual void onSymbol(RawSymbol&& symbol) = 0;
    virtual void onModule(ModuleRange&& module) = 0;
    virtual void onTraceEnd(const TraceSummary& summary) = 0;
};

/// Anything that can replay a recorded session.
class ITraceSource {
public:
    virtual ~ITraceSource() = default;

    /// Streams every record to @p visitor. Returns an Error for unreadable or
    /// malformed traces; a truncated-but-valid prefix is replayed and reported
    /// through TraceSummary instead (a crashed target still yields data).
    [[nodiscard]] virtual Status replay(ITraceVisitor& visitor) = 0;
};

}  // namespace leakhunter::tracker
