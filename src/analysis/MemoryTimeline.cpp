#include "leakhunter/analysis/MemoryTimeline.hpp"

#include <algorithm>
#include <string_view>

#include <fmt/format.h>

namespace leakhunter::analysis {
namespace {

/// A run that ends holding this much of its peak never gave the memory back.
constexpr double kNearPeakFraction = 0.90;

/// Above this, the program is recycling hard enough to be worth a remark.
constexpr double kNotableTurnover = 4.0;

}  // namespace

std::string MemoryTimeline::summary() const {
    if (empty()) {
        return "no timeline: nothing was allocated, or the trace carried no timings";
    }

    const std::uint64_t finalBytes = samples.back().liveBytes;

    if (truncated) {
        // Say what is missing before saying anything about the shape. A reader
        // who takes the plateau at face value here draws the exact wrong
        // conclusion, so the caveat cannot be a footnote.
        return fmt::format(
            "Peaked at {} within the first {:.0f}% of the run -- the timeline stops there, so "
            "anything after it is not shown and the flat tail is missing data, not stable memory.",
            formatBytes(peakBytes), coverage * 100.0);
    }

    if (endedNearPeak && peakBytes > 0) {
        return fmt::format(
            "Live memory climbed to {} and ended at {}: it went up and did not come back down, "
            "which is what a leak looks like from outside the process.",
            formatBytes(peakBytes), formatBytes(finalBytes));
    }

    if (turnover >= kNotableTurnover && peakBytes > 0) {
        return fmt::format(
            "{:.1f}x turnover: {} passed through the allocator, but never more than {} was held at "
            "once, and the run ended holding {}. That is healthy recycling -- worth knowing only "
            "because it means the allocator is doing real work on this path.",
            turnover,
            formatBytes(static_cast<std::uint64_t>(turnover * static_cast<double>(peakBytes))),
            formatBytes(peakBytes), formatBytes(finalBytes));
    }

    return fmt::format("Peaked at {} ({} blocks), ended holding {}.", formatBytes(peakBytes),
                       peakBlocks, formatBytes(finalBytes));
}

std::string renderSparkline(const MemoryTimeline& timeline, std::size_t width) {
    static constexpr std::string_view kRamp = " .:-=+*#";

    if (timeline.empty() || width == 0 || timeline.peakBytes == 0) {
        return {};
    }

    std::string line;
    line.reserve(width);

    for (std::size_t column = 0; column < width; ++column) {
        // Map the column onto the samples rather than the samples onto columns:
        // the two counts are unrelated, and this way a short timeline stretches
        // instead of leaving gaps.
        const std::size_t index = std::min(
            timeline.samples.size() - 1,
            static_cast<std::size_t>((static_cast<double>(column) *
                                      static_cast<double>(timeline.samples.size())) /
                                     static_cast<double>(width)));

        const double fraction = static_cast<double>(timeline.samples[index].liveBytes) /
                                static_cast<double>(timeline.peakBytes);

        // Anything non-zero gets at least the lightest visible mark. A column
        // holding one byte and a column holding nothing must not look alike.
        std::size_t level = static_cast<std::size_t>(fraction * static_cast<double>(kRamp.size() - 1));
        if (timeline.samples[index].liveBytes > 0 && level == 0) {
            level = 1;
        }
        line.push_back(kRamp[std::min(level, kRamp.size() - 1)]);
    }

    return line;
}

MemoryTimeline buildTimeline(std::vector<MemoryEvent> events, std::uint64_t totalBytesAllocated,
                             bool truncated, std::uint64_t runDurationNs,
                             std::size_t sampleCount) {
    MemoryTimeline timeline;
    timeline.truncated = truncated;

    if (events.empty() || sampleCount == 0) {
        return timeline;
    }

    // Events are close to ordered but not exactly: a thread can be preempted
    // between taking its timestamp and its record being written. Sorting is
    // cheap next to the tracing that produced them.
    std::sort(events.begin(), events.end(), [](const MemoryEvent& lhs, const MemoryEvent& rhs) {
        return lhs.timestampNs < rhs.timestampNs;
    });

    const std::uint64_t firstNs = events.front().timestampNs;
    const std::uint64_t lastNs = events.back().timestampNs;
    const std::uint64_t span = lastNs > firstNs ? lastNs - firstNs : 0;

    if (truncated && runDurationNs > 0) {
        timeline.coverage =
            std::min(1.0, static_cast<double>(lastNs) / static_cast<double>(runDurationNs));
    }

    std::int64_t live = 0;
    std::int64_t liveBlocks = 0;
    std::size_t cursor = 0;

    // Tracked per event rather than per bucket: a spike that rises and falls
    // inside one bucket is exactly what a reader wants to know about, and
    // bucketing would average it away.
    const auto observe = [&](std::uint64_t when) {
        if (live > 0 && static_cast<std::uint64_t>(live) > timeline.peakBytes) {
            timeline.peakBytes = static_cast<std::uint64_t>(live);
            timeline.peakBlocks = static_cast<std::uint64_t>(std::max<std::int64_t>(liveBlocks, 0));
            timeline.peakAtNs = when;
        }
    };

    if (span == 0) {
        // Everything landed on one timestamp: a single sample is the honest
        // rendering, and dividing by the span would not be.
        for (const MemoryEvent& event : events) {
            live += event.deltaBytes;
            liveBlocks += event.deltaBytes > 0 ? 1 : -1;
            observe(lastNs);
        }
        timeline.samples.push_back({lastNs,
                                    static_cast<std::uint64_t>(std::max<std::int64_t>(live, 0)),
                                    static_cast<std::uint64_t>(std::max<std::int64_t>(liveBlocks, 0))});
    } else {
        timeline.samples.reserve(sampleCount);

        for (std::size_t bucket = 0; bucket < sampleCount; ++bucket) {
            const std::uint64_t bucketEnd =
                firstNs + static_cast<std::uint64_t>(
                              (static_cast<double>(span) * static_cast<double>(bucket + 1)) /
                              static_cast<double>(sampleCount));

            while (cursor < events.size() && events[cursor].timestampNs <= bucketEnd) {
                live += events[cursor].deltaBytes;
                liveBlocks += events[cursor].deltaBytes > 0 ? 1 : -1;
                observe(events[cursor].timestampNs);
                ++cursor;
            }

            timeline.samples.push_back(
                {bucketEnd, static_cast<std::uint64_t>(std::max<std::int64_t>(live, 0)),
                 static_cast<std::uint64_t>(std::max<std::int64_t>(liveBlocks, 0))});
        }
    }

    if (timeline.peakBytes > 0) {
        timeline.turnover =
            static_cast<double>(totalBytesAllocated) / static_cast<double>(timeline.peakBytes);

        // Deliberately not computed on a truncated timeline: the tail is
        // missing, so "ended near its peak" would be a claim about data we
        // never saw.
        timeline.endedNearPeak =
            !truncated && static_cast<double>(timeline.samples.back().liveBytes) >=
                              static_cast<double>(timeline.peakBytes) * kNearPeakFraction;
    }

    return timeline;
}

}  // namespace leakhunter::analysis
