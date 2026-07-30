/// @file MemoryTimeline.hpp
/// @brief Live memory over the life of the run.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "leakhunter/core/Types.hpp"

namespace leakhunter::analysis {

struct MemorySample {
    std::uint64_t timestampNs = 0;  ///< end of the bucket this sample covers
    std::uint64_t liveBytes = 0;
    std::uint64_t liveBlocks = 0;
};

/// What the heap did, rather than what it was left holding.
///
/// The trace has always carried a timestamp on every free; until now nothing
/// read them, so the report could say what was left over but never what the
/// program cost while it ran. A program that peaks at 900 MiB and exits holding
/// 4 KiB has no leak and a very real problem, and until this existed the report
/// called it clean and said nothing else.
struct MemoryTimeline {
    std::vector<MemorySample> samples;

    std::uint64_t peakBytes = 0;
    std::uint64_t peakBlocks = 0;
    std::uint64_t peakAtNs = 0;

    /// Bytes that passed through the allocator, divided by the most ever held
    /// at one moment.
    ///
    /// A large number is not a defect -- it means the program recycles, which
    /// is what a well-behaved program does. It *is* allocator pressure, and it
    /// is the thing a leak report otherwise says nothing about: a program that
    /// churns 4 GiB through a 2 MiB working set has a performance question even
    /// when it leaks nothing at all.
    double turnover = 0.0;

    /// True when the run ended holding near its peak, which is what a leak
    /// looks like from outside the process. Never set on a truncated timeline,
    /// where the tail is missing and the claim would be unfounded.
    bool endedNearPeak = false;

    /// Events the registry could not keep. A truncated timeline covers only the
    /// beginning of the run.
    ///
    /// This matters more than an ordinary cap, because the failure is not
    /// "less detail" -- a truncated series shows memory rising and then going
    /// flat, which is exactly what "the leak stopped" looks like. Every
    /// renderer has to say so instead of drawing a reassuring plateau.
    bool truncated = false;

    /// Fraction of the run the samples actually cover: 1.0 unless truncated.
    double coverage = 1.0;

    [[nodiscard]] bool empty() const noexcept { return samples.empty(); }

    /// A one-line reading of the shape, for people rather than plots.
    [[nodiscard]] std::string summary() const;
};

/// Buckets @p events into at most @p sampleCount samples.
///
/// Events arrive roughly ordered -- appended as the trace is replayed -- but a
/// thread can be preempted between taking its timestamp and being written, so
/// this sorts. @p sampleCount is a rendering concern: a chart cannot show a
/// million points and a reader cannot read them.
///
/// @param runDurationNs  the full run, used to work out how much of it a
///                       truncated event list covers. Zero when unknown.
[[nodiscard]] MemoryTimeline buildTimeline(std::vector<MemoryEvent> events,
                                           std::uint64_t totalBytesAllocated, bool truncated,
                                           std::uint64_t runDurationNs = 0,
                                           std::size_t sampleCount = 120);

/// A one-line plot of live memory, scaled to the peak.
///
/// ASCII on purpose. Unicode block characters draw a prettier curve, but this
/// output lands in CI logs, `tee`d files and terminals with every imaginable
/// encoding, and a row of replacement characters is worth less than a rough
/// shape that always renders. The ramp runs ` .:-=+*#`, lightest to heaviest.
[[nodiscard]] std::string renderSparkline(const MemoryTimeline& timeline, std::size_t width = 60);

}  // namespace leakhunter::analysis
