/// @file Baseline.hpp
/// @brief Comparing a run against a previous one, so CI can fail on *new* leaks.
///
/// A gate that fails on any leak is unusable on an existing codebase. The first
/// run on a real project returns four thousand findings, the team switches the
/// tool off that week, and it protects nothing afterwards. Every static-analysis
/// product that survives contact with legacy code solves this the same way: hold
/// the line where it is, and fail only on what got worse.
///
/// The comparison is by **call site**, not by address or by count. Addresses
/// move between runs, and counts wobble on anything non-deterministic; the
/// function that allocated is the part that is stable enough to gate on.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "leakhunter/core/Error.hpp"

namespace leakhunter::analysis {

struct LeakReport;

/// One site's fate between the baseline and this run.
struct SiteDelta {
    std::string function;
    std::string location;

    std::uint64_t baselineBytes = 0;
    std::uint64_t baselineCount = 0;
    std::uint64_t currentBytes = 0;
    std::uint64_t currentCount = 0;

    [[nodiscard]] bool isNew() const noexcept { return baselineCount == 0 && currentCount > 0; }
    [[nodiscard]] bool isFixed() const noexcept { return baselineCount > 0 && currentCount == 0; }
    /// Exact comparison. The tolerance is applied by compareToBaseline(),
    /// not here: a predicate named isWorse() that quietly ignores a 4% increase
    /// would be a trap for anyone reading the struct.
    [[nodiscard]] bool isWorse() const noexcept {
        return baselineCount > 0 && currentCount > baselineCount;
    }
    [[nodiscard]] bool isBetter() const noexcept {
        return currentCount > 0 && currentCount < baselineCount;
    }

    [[nodiscard]] std::int64_t byteDelta() const noexcept {
        return static_cast<std::int64_t>(currentBytes) - static_cast<std::int64_t>(baselineBytes);
    }
};

/// What changed, in the terms a pull request is judged on.
struct BaselineDiff {
    bool loaded = false;      ///< a baseline was supplied and parsed
    std::string baselinePath;
    std::string baselineGeneratedAt;

    std::vector<SiteDelta> newSites;
    std::vector<SiteDelta> worseSites;
    std::vector<SiteDelta> fixedSites;

    std::uint64_t unchangedSites = 0;

    /// Growth below this percentage was not counted as a regression.
    ///
    /// Needed by any program whose allocation count is not perfectly
    /// reproducible -- a thread pool, a hash map that rehashes, anything
    /// reading a clock. Without it such a target fails its own gate at random,
    /// and a gate that fires at random is worse than no gate: the team learns
    /// to re-run the build until it passes.
    double tolerancePercent = 0.0;

    /// Sites that grew, but stayed inside the tolerance. Reported so the
    /// allowance is never invisible -- a threshold nobody can see is a
    /// threshold nobody can question.
    std::vector<SiteDelta> withinTolerance;

    /// Mismatched frees are gated too, by count: they are undefined behaviour,
    /// and letting a build introduce one because the baseline had others would
    /// defeat the purpose.
    std::uint64_t baselineMismatches = 0;
    std::uint64_t currentMismatches = 0;

    [[nodiscard]] bool regressed() const noexcept {
        return !newSites.empty() || !worseSites.empty() ||
               currentMismatches > baselineMismatches;
    }

    /// True when nothing got worse *and* something got better -- worth saying
    /// out loud, because a gate that only ever scolds gets ignored.
    [[nodiscard]] bool improved() const noexcept {
        return !regressed() && (!fixedSites.empty() || currentMismatches < baselineMismatches);
    }
};

/// Loads a previously written `report.json` and diffs @p report against it.
///
/// Returns an error when the file is missing or unreadable rather than silently
/// treating it as empty: a mistyped baseline path would otherwise turn every
/// pre-existing leak into a brand new one and fail the build for the wrong
/// reason.
/// @param tolerancePercent  growth at an existing site below this percentage is
///                          not a regression. 0 means exact.
[[nodiscard]] Result<BaselineDiff> compareToBaseline(const LeakReport& report,
                                                     const std::string& baselinePath,
                                                     double tolerancePercent = 0.0);

}  // namespace leakhunter::analysis
