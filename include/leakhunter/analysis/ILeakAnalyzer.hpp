/// @file ILeakAnalyzer.hpp
/// @brief Live allocations -> analysed, grouped leak report.

#pragma once

#include <vector>

#include "leakhunter/analysis/LeakReport.hpp"
#include "leakhunter/core/Types.hpp"

namespace leakhunter::analysis {

class ILeakAnalyzer {
public:
    virtual ~ILeakAnalyzer() = default;

    /// @param liveAllocations everything still outstanding at process exit
    /// @param mismatches      blocks released through an incompatible entry point
    /// @param stats           counters gathered during the run
    [[nodiscard]] virtual LeakReport analyze(std::vector<AllocationInfo> liveAllocations,
                                             std::vector<MismatchedFree> mismatches,
                                             const SessionStats& stats) = 0;
};

}  // namespace leakhunter::analysis
