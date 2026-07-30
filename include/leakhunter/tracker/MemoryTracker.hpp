/// @file MemoryTracker.hpp
/// @brief Host-side half of the memory tracker.
///
/// The tracker spans two processes:
///   * `libleakhunter_agent.so` (src/agent) intercepts allocations inside the
///     target and appends records to a trace file;
///   * this class replays that trace and routes each record to the registry
///     (allocation book-keeping) and the symbol resolver (name lookups).
///
/// Splitting it this way keeps the injected code minimal -- the part that must
/// not allocate stays small, everything analytical lives here.

#pragma once

#include "leakhunter/core/Types.hpp"
#include "leakhunter/registry/AllocationRegistry.hpp"
#include "leakhunter/symbols/SymbolResolver.hpp"
#include "leakhunter/tracker/ITraceSource.hpp"

namespace leakhunter::tracker {

class MemoryTracker final : public ITraceVisitor {
public:
    MemoryTracker(registry::AllocationRegistry& registry, symbols::SymbolResolver& resolver);

    /// Replays @p source into the registry and resolver.
    [[nodiscard]] Status consume(ITraceSource& source);

    [[nodiscard]] bool sawTraceEnd() const noexcept { return sawEnd_; }
    [[nodiscard]] const TraceInfo& traceInfo() const noexcept { return info_; }

    // ITraceVisitor
    void onTraceBegin(const TraceInfo& info) override;
    void onAllocation(AllocationInfo&& allocation) override;
    void onDeallocation(std::uint64_t address, std::uint64_t timestampNs, ReleaseKind releasedBy,
                        std::uint64_t threadId) override;
    void onSymbol(RawSymbol&& symbol) override;
    void onModule(ModuleRange&& module) override;
    void onTraceEnd(const TraceSummary& summary) override;

private:
    registry::AllocationRegistry& registry_;
    symbols::SymbolResolver& resolver_;
    TraceInfo info_;
    bool sawEnd_ = false;
};

}  // namespace leakhunter::tracker
