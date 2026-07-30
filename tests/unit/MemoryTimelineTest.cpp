/// Reconstructing what the heap did from the alloc/free stream.
///
/// The timeline is the one part of the report a reader will *look* at rather
/// than read, and a plausible-looking wrong chart is worse than no chart. So
/// most of these check arithmetic against answers worked out by hand, and the
/// rest check that it declines to claim things the data does not support.

#include <string>
#include <vector>

#include "TestFramework.hpp"
#include "leakhunter/analysis/MemoryTimeline.hpp"

using leakhunter::MemoryEvent;
using leakhunter::analysis::buildTimeline;
using leakhunter::analysis::MemoryTimeline;

namespace {

constexpr std::uint64_t kMs = 1'000'000ULL;

}  // namespace

LH_TEST(Timeline, an_empty_event_list_produces_an_empty_timeline) {
    const MemoryTimeline timeline = buildTimeline({}, 0, /*truncated=*/false);
    LH_CHECK(timeline.empty());
    LH_CHECK_EQ(timeline.peakBytes, std::uint64_t{0});
    // Must not divide by a zero peak.
    LH_CHECK_EQ(timeline.turnover, 0.0);
}

LH_TEST(Timeline, live_bytes_follow_the_deltas) {
    // +100 +200 -100 => 200 held at the end, 300 at the peak.
    const std::vector<MemoryEvent> events{
        {1 * kMs, 100}, {2 * kMs, 200}, {3 * kMs, -100}};

    const MemoryTimeline timeline = buildTimeline(events, 300, /*truncated=*/false);
    LH_CHECK_EQ(timeline.peakBytes, std::uint64_t{300});
    LH_CHECK_EQ(timeline.samples.back().liveBytes, std::uint64_t{200});
    LH_CHECK_EQ(timeline.samples.back().liveBlocks, std::uint64_t{1});
}

LH_TEST(Timeline, a_spike_inside_one_bucket_still_registers_as_the_peak) {
    // The reason the peak is tracked per event rather than per sample. This
    // rises to 1 MiB and falls back inside a single bucket; a chart that only
    // remembered bucket ends would show a flat line at 1 KiB and hide the
    // allocation that actually decided the process's RSS.
    std::vector<MemoryEvent> events{{0, 1024}};
    events.push_back({500, 1024 * 1024});
    events.push_back({600, -1024 * 1024});
    for (int i = 1; i <= 200; ++i) {
        events.push_back({static_cast<std::uint64_t>(i) * kMs, 0});
    }

    const MemoryTimeline timeline = buildTimeline(events, 1024 * 1024 + 1024, /*truncated=*/false);
    LH_CHECK_EQ(timeline.peakBytes, std::uint64_t{1024 * 1024 + 1024});
    LH_CHECK_EQ(timeline.peakAtNs, std::uint64_t{500});
    // ...and the final state is still reported honestly.
    LH_CHECK_EQ(timeline.samples.back().liveBytes, std::uint64_t{1024});
}

LH_TEST(Timeline, out_of_order_events_are_sorted_before_bucketing) {
    // Threads are preempted between taking a timestamp and being written, so
    // the trace is only approximately ordered. Unsorted, the running total
    // would go negative and the peak would be wrong.
    const std::vector<MemoryEvent> events{
        {3 * kMs, -100}, {1 * kMs, 100}, {2 * kMs, 200}};

    const MemoryTimeline timeline = buildTimeline(events, 300, /*truncated=*/false);
    LH_CHECK_EQ(timeline.peakBytes, std::uint64_t{300});
    LH_CHECK_EQ(timeline.samples.back().liveBytes, std::uint64_t{200});
}

LH_TEST(Timeline, everything_at_one_instant_yields_one_sample) {
    // A program fast enough that every timestamp collides. Bucketing over a
    // zero span would divide by zero; one sample is the honest rendering.
    const std::vector<MemoryEvent> events{{42, 100}, {42, 200}, {42, -50}};

    const MemoryTimeline timeline = buildTimeline(events, 300, /*truncated=*/false);
    LH_CHECK_EQ(timeline.samples.size(), std::size_t{1});
    LH_CHECK_EQ(timeline.samples[0].liveBytes, std::uint64_t{250});
    LH_CHECK_EQ(timeline.peakBytes, std::uint64_t{300});
}

LH_TEST(Timeline, a_run_that_ends_at_its_peak_is_flagged) {
    // The shape of a leak seen from outside: up, and never back down.
    std::vector<MemoryEvent> events;
    for (int i = 0; i < 100; ++i) {
        events.push_back({static_cast<std::uint64_t>(i) * kMs, 1024});
    }

    const MemoryTimeline timeline = buildTimeline(events, 100 * 1024, /*truncated=*/false);
    LH_CHECK(timeline.endedNearPeak);
    LH_CHECK(timeline.summary().find("did not come back down") != std::string::npos);
}

LH_TEST(Timeline, a_run_that_gives_the_memory_back_is_not_flagged) {
    std::vector<MemoryEvent> events;
    for (int i = 0; i < 100; ++i) {
        events.push_back({static_cast<std::uint64_t>(i) * kMs, 1024});
    }
    for (int i = 0; i < 100; ++i) {
        events.push_back({static_cast<std::uint64_t>(100 + i) * kMs, -1024});
    }

    const MemoryTimeline timeline = buildTimeline(events, 100 * 1024, /*truncated=*/false);
    LH_CHECK(!timeline.endedNearPeak);
    LH_CHECK_EQ(timeline.samples.back().liveBytes, std::uint64_t{0});
}

LH_TEST(Timeline, turnover_is_bytes_allocated_over_the_high_water_mark) {
    // 10 allocations of 1 KiB, each freed before the next: 10 KiB through the
    // allocator, never more than 1 KiB held. Exactly 10x, by hand.
    std::vector<MemoryEvent> events;
    for (int i = 0; i < 10; ++i) {
        events.push_back({static_cast<std::uint64_t>(i * 2) * kMs, 1024});
        events.push_back({static_cast<std::uint64_t>(i * 2 + 1) * kMs, -1024});
    }

    const MemoryTimeline timeline = buildTimeline(events, 10 * 1024, /*truncated=*/false);
    LH_CHECK_EQ(timeline.peakBytes, std::uint64_t{1024});
    LH_CHECK(timeline.turnover > 9.99);
    LH_CHECK(timeline.turnover < 10.01);
    // Healthy recycling has to read as healthy, not as a warning.
    LH_CHECK(timeline.summary().find("turnover") != std::string::npos);
    LH_CHECK(timeline.summary().find("healthy") != std::string::npos);
}

// --- truncation, where a wrong chart is actively misleading -----------------

LH_TEST(Timeline, a_truncated_timeline_never_claims_the_run_ended_near_its_peak) {
    // These events *look* like a leak that ran to the end. They are the first
    // slice of a much longer run, and the flat tail is missing data. Claiming
    // "ended near peak" here would be a statement about samples that do not
    // exist.
    std::vector<MemoryEvent> events;
    for (int i = 0; i < 100; ++i) {
        events.push_back({static_cast<std::uint64_t>(i) * kMs, 1024});
    }

    const MemoryTimeline timeline =
        buildTimeline(events, 100 * 1024, /*truncated=*/true, /*runDurationNs=*/1000 * kMs);
    LH_CHECK(timeline.truncated);
    LH_CHECK(!timeline.endedNearPeak);
}

LH_TEST(Timeline, a_truncated_timeline_reports_how_much_of_the_run_it_covers) {
    std::vector<MemoryEvent> events;
    for (int i = 0; i < 100; ++i) {
        events.push_back({static_cast<std::uint64_t>(i) * kMs, 1024});
    }

    // Last event at 99 ms of a 1000 ms run: just under 10%.
    const MemoryTimeline timeline =
        buildTimeline(events, 100 * 1024, /*truncated=*/true, /*runDurationNs=*/1000 * kMs);
    LH_CHECK(timeline.coverage > 0.09);
    LH_CHECK(timeline.coverage < 0.10);

    const std::string summary = timeline.summary();
    LH_CHECK(summary.find("missing data") != std::string::npos);
    LH_CHECK(summary.find("not stable memory") != std::string::npos);
}

LH_TEST(Timeline, coverage_never_exceeds_one_when_the_duration_is_unreliable) {
    // A stopped target's wall clock can round below the last recorded event.
    // Coverage above 100% would be nonsense on a chart axis.
    const std::vector<MemoryEvent> events{{0, 1024}, {1000 * kMs, 1024}};
    const MemoryTimeline timeline =
        buildTimeline(events, 2048, /*truncated=*/true, /*runDurationNs=*/10 * kMs);
    LH_CHECK_EQ(timeline.coverage, 1.0);
}

LH_TEST(Timeline, the_sample_count_is_respected) {
    std::vector<MemoryEvent> events;
    for (int i = 0; i < 10'000; ++i) {
        events.push_back({static_cast<std::uint64_t>(i) * kMs, 64});
    }

    const MemoryTimeline timeline =
        buildTimeline(events, 640'000, /*truncated=*/false, /*runDurationNs=*/0, /*samples=*/40);
    LH_CHECK_EQ(timeline.samples.size(), std::size_t{40});
    // Bucketing must not lose bytes: the last sample is the true final state.
    LH_CHECK_EQ(timeline.samples.back().liveBytes, std::uint64_t{640'000});
}

LH_TEST(Timeline, samples_are_ordered_in_time) {
    // The renderers plot these left to right without re-sorting.
    std::vector<MemoryEvent> events;
    for (int i = 0; i < 500; ++i) {
        events.push_back({static_cast<std::uint64_t>(i) * kMs, 64});
    }

    const MemoryTimeline timeline = buildTimeline(events, 32'000, /*truncated=*/false);
    for (std::size_t i = 1; i < timeline.samples.size(); ++i) {
        LH_CHECK(timeline.samples[i].timestampNs >= timeline.samples[i - 1].timestampNs);
    }
}

LH_TEST(Timeline, live_bytes_never_go_negative_on_an_incomplete_stream) {
    // Blocks allocated before the agent was live are freed without a matching
    // allocation. The running total must clamp, not wrap around uint64.
    const std::vector<MemoryEvent> events{{1 * kMs, -4096}, {2 * kMs, 100}};

    const MemoryTimeline timeline = buildTimeline(events, 100, /*truncated=*/false);
    for (const auto& sample : timeline.samples) {
        LH_CHECK(sample.liveBytes < std::uint64_t{1} << 40);
    }
}
