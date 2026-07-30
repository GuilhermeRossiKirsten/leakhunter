/// @file LeakAnalyzer.hpp
/// @brief Default leak analysis: symbolise, blame, group, sort.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "leakhunter/analysis/ILeakAnalyzer.hpp"
#include "leakhunter/analysis/SuppressionSet.hpp"
#include "leakhunter/symbols/ISymbolResolver.hpp"

namespace leakhunter::analysis {

struct AnalyzerConfig {
    /// Leaks below this size are excluded from the listing (still counted).
    std::uint64_t minLeakSize = 0;

    /// Cap on leaks listed individually; groups always cover everything.
    std::size_t maxDetailedLeaks = 5000;

    /// List allocations the C runtime never frees alongside application leaks.
    bool includeRuntimeLeaks = false;

    /// Report blocks released through the wrong entry point. On by default: it
    /// costs nothing at run time and the evidence is already in the trace.
    bool detectMismatchedFrees = true;

    /// Cap on mismatches listed individually.
    std::size_t maxDetailedMismatches = 200;

    /// Rules for leaks that are known and accepted. Not owned; must outlive the
    /// analyzer, and is mutated (hit counts) during analysis.
    SuppressionSet* suppressions = nullptr;
};

/// Every allocation still live at exit is a leak -- LeakHunter deliberately
/// does not attempt reachability analysis (that is Valgrind's job). The value
/// added here is attribution: finding the frame a developer can act on.
class LeakAnalyzer final : public ILeakAnalyzer {
public:
    LeakAnalyzer(const symbols::ISymbolResolver& resolver, AnalyzerConfig config = {});

    [[nodiscard]] LeakReport analyze(std::vector<AllocationInfo> liveAllocations,
                                     std::vector<MismatchedFree> mismatches,
                                     const SessionStats& stats) override;

    /// Index of the first frame that is neither the agent nor an allocator
    /// entry point. Exposed for testing; falls back to 0 when every frame
    /// looks like infrastructure.
    [[nodiscard]] static std::size_t findResponsibleFrame(const StackTrace& trace);

    /// True for `malloc`, `operator new`, allocator internals and agent frames.
    [[nodiscard]] static bool isAllocatorFrame(const StackFrame& frame);

    /// Classifies a stack by the module that actually requested the memory:
    /// the innermost frame that is neither our agent nor an allocator symbol.
    /// Inside libc or the loader means the C runtime asked for it.
    [[nodiscard]] static LeakOrigin classifyOrigin(const StackTrace& trace);

private:
    void buildGroups(LeakReport& report) const;
    void addMismatches(LeakReport& report, std::vector<MismatchedFree> mismatches) const;
    void summariseSuppressions(
        LeakReport& report,
        const std::unordered_map<std::size_t, std::uint64_t>& ruleBytes) const;

    const symbols::ISymbolResolver& resolver_;
    AnalyzerConfig config_;
};

}  // namespace leakhunter::analysis
