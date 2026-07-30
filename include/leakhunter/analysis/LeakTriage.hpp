/// @file LeakTriage.hpp
/// @brief Turning "where and how much" into "how bad, and what now".

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace leakhunter::analysis {

struct LeakReport;

/// When a site's leaks happened, relative to the run.
///
/// This is the difference between a finding you act on tonight and one you can
/// schedule. A block leaked once at start-up costs a fixed amount for ever; the
/// same block leaked on every request costs that amount per request.
enum class LeakPattern : std::uint8_t {
    Unknown = 0,  ///< not enough timing information to say
    OneShot = 1,  ///< a single block
    Startup = 2,  ///< all of them early in the run: a fixed cost
    Burst = 3,    ///< clustered, but not at start-up: tied to some event
    Steady = 4,   ///< spread across the run: **grows with uptime**
};

[[nodiscard]] std::string_view toString(LeakPattern pattern) noexcept;

/// A one-line, human-readable characterisation of a pattern.
[[nodiscard]] std::string_view describe(LeakPattern pattern) noexcept;

/// What a reader needs after "there is a leak here".
struct LeakTriage {
    LeakPattern pattern = LeakPattern::Unknown;

    /// Extrapolated from the observed rate. Only meaningful for Steady: for a
    /// site that allocated once at start-up, "bytes per hour" is a fiction.
    double bytesPerHour = 0.0;

    std::uint64_t firstSeenNs = 0;  ///< relative to process start
    std::uint64_t lastSeenNs = 0;

    /// True when the timings come from fewer blocks than the site actually
    /// leaked -- `--min-leak-size` and the detail cap both do this. The rate is
    /// still computed from the site's full byte count, but the *window* it is
    /// divided by comes from a sample, so it is a lower bound on precision.
    bool sampleIsPartial = false;

    /// What to do about it, in the order worth reading.
    std::vector<std::string> advice;

    /// A rule that would silence this exact site, in the format
    /// `--suppressions` accepts. Generated so that accepting a known leak is
    /// one copy away -- the alternative is people ignoring the whole report.
    std::string suppressionRule;

    [[nodiscard]] bool empty() const noexcept {
        return pattern == LeakPattern::Unknown && advice.empty();
    }
};

/// Fills in `triage` on every group of @p report.
///
/// Pure: reads the report and writes back into it, no I/O, no clock. Everything
/// it needs is already recorded -- per-leak timestamps, the run's duration, and
/// the allocating entry point.
void triageLeaks(LeakReport& report);

}  // namespace leakhunter::analysis
