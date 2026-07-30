/// Registry behaviour: the leak/no-leak decision itself.

#include "TestFramework.hpp"
#include "leakhunter/registry/AllocationRegistry.hpp"

using leakhunter::AllocationInfo;
using leakhunter::AllocationKind;
using leakhunter::ReleaseKind;
using leakhunter::registry::AllocationRegistry;

namespace {

AllocationInfo makeAllocation(std::uint64_t address, std::uint64_t size,
                              std::uint64_t threadId = 1,
                              AllocationKind kind = AllocationKind::Malloc) {
    AllocationInfo allocation;
    allocation.address = address;
    allocation.size = size;
    allocation.threadId = threadId;
    allocation.timestampNs = address;  // deterministic and unique enough
    allocation.kind = kind;
    allocation.callStack = {0x1000, 0x2000};
    return allocation;
}

/// The common case, where how it was freed is not what the test is about.
bool freeIt(AllocationRegistry& registry, std::uint64_t address,
            ReleaseKind kind = ReleaseKind::Free) {
    return registry.recordDeallocation(address, /*timestampNs=*/0, kind, /*threadId=*/1);
}

}  // namespace

LH_TEST(Registry, allocation_without_free_is_a_leak) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));

    LH_CHECK_EQ(registry.liveCount(), std::size_t{1});

    const auto live = registry.takeLiveAllocations();
    LH_CHECK_EQ(live.size(), std::size_t{1});
    LH_CHECK_EQ(live[0].address, std::uint64_t{0xAAAA});
    LH_CHECK_EQ(live[0].size, std::uint64_t{1024});
}

LH_TEST(Registry, matched_free_leaves_nothing_behind) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));

    LH_CHECK(freeIt(registry, 0xAAAA));
    LH_CHECK_EQ(registry.liveCount(), std::size_t{0});
    LH_CHECK_EQ(registry.stats().totalBytesFreed, std::uint64_t{1024});
}

LH_TEST(Registry, multiple_leaks_are_all_retained) {
    AllocationRegistry registry;
    for (std::uint64_t i = 1; i <= 50; ++i) {
        registry.recordAllocation(makeAllocation(0x1000 + i * 0x100, i * 8));
    }
    // Free every other one.
    for (std::uint64_t i = 1; i <= 50; i += 2) {
        freeIt(registry, 0x1000 + i * 0x100);
    }

    LH_CHECK_EQ(registry.liveCount(), std::size_t{25});
    LH_CHECK_EQ(registry.stats().totalAllocations, std::uint64_t{50});
    LH_CHECK_EQ(registry.stats().totalDeallocations, std::uint64_t{25});
}

LH_TEST(Registry, free_of_an_unknown_pointer_is_counted_not_fatal) {
    AllocationRegistry registry;

    LH_CHECK(!freeIt(registry, 0xDEAD));
    LH_CHECK_EQ(registry.stats().untrackedFrees, std::uint64_t{1});
    LH_CHECK_EQ(registry.liveCount(), std::size_t{0});
}

LH_TEST(Registry, free_of_null_is_not_a_mismatch) {
    AllocationRegistry registry;

    LH_CHECK(freeIt(registry, 0));
    LH_CHECK_EQ(registry.stats().untrackedFrees, std::uint64_t{0});
}

LH_TEST(Registry, reused_address_replaces_the_stale_record) {
    // The allocator handed the same address out twice without us seeing the
    // free in between. The newer allocation must win, otherwise a live block
    // would be blamed on a stack trace from a previous life.
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0xBEEF, 100, /*threadId=*/7));
    registry.recordAllocation(makeAllocation(0xBEEF, 200, /*threadId=*/9));

    LH_CHECK_EQ(registry.liveCount(), std::size_t{1});

    const auto live = registry.takeLiveAllocations();
    LH_CHECK_EQ(live[0].size, std::uint64_t{200});
    LH_CHECK_EQ(live[0].threadId, std::uint64_t{9});
}

LH_TEST(Registry, peak_live_bytes_tracks_the_high_water_mark) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0x10, 1000));
    registry.recordAllocation(makeAllocation(0x20, 3000));  // peak = 4000
    freeIt(registry, 0x20);
    registry.recordAllocation(makeAllocation(0x30, 500));

    LH_CHECK_EQ(registry.stats().peakLiveBytes, std::uint64_t{4000});
}

LH_TEST(Registry, failed_allocation_is_counted_but_never_leaks) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0, 4096));  // malloc returned null

    LH_CHECK_EQ(registry.stats().totalAllocations, std::uint64_t{1});
    LH_CHECK_EQ(registry.liveCount(), std::size_t{0});
}

// --- mismatched frees ------------------------------------------------------

namespace {

/// Allocates one block with @p allocated, releases it with @p released, and
/// returns how many mismatches that produced.
///
/// The extra correct new/delete pair is not decoration: the registry refuses to
/// report new/free pairings unless it has seen `operator delete` interposed at
/// least once, because "new hooked but delete not" is a real configuration that
/// would otherwise flood the report. See mismatchDetectionIsTrustworthy().
std::uint64_t pairing(AllocationKind allocated, ReleaseKind released) {
    AllocationRegistry registry;

    registry.recordAllocation(makeAllocation(0x9000, 8, 1, AllocationKind::New));
    freeIt(registry, 0x9000, ReleaseKind::Delete);

    registry.recordAllocation(makeAllocation(0x1000, 64, 1, allocated));
    freeIt(registry, 0x1000, released);

    return registry.takeMismatchedFrees().size();
}

}  // namespace

LH_TEST(Mismatch, the_legal_pairings_are_silent) {
    LH_CHECK_EQ(pairing(AllocationKind::Malloc, ReleaseKind::Free), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::Calloc, ReleaseKind::Free), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::Realloc, ReleaseKind::Free), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::Malloc, ReleaseKind::Realloc), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::New, ReleaseKind::Delete), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::NewArray, ReleaseKind::DeleteArray), std::uint64_t{0});

    // POSIX leaves both of these undefined, glibc supports them, and treating
    // them as bugs would cost more in noise than it earns. Deliberate.
    LH_CHECK_EQ(pairing(AllocationKind::AlignedAlloc, ReleaseKind::Free), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::AlignedAlloc, ReleaseKind::Realloc), std::uint64_t{0});
}

LH_TEST(Mismatch, crossing_the_c_and_cpp_allocators_is_reported) {
    LH_CHECK_EQ(pairing(AllocationKind::New, ReleaseKind::Free), std::uint64_t{1});
    LH_CHECK_EQ(pairing(AllocationKind::NewArray, ReleaseKind::Free), std::uint64_t{1});
    LH_CHECK_EQ(pairing(AllocationKind::Malloc, ReleaseKind::Delete), std::uint64_t{1});
    LH_CHECK_EQ(pairing(AllocationKind::Calloc, ReleaseKind::DeleteArray), std::uint64_t{1});
    LH_CHECK_EQ(pairing(AllocationKind::New, ReleaseKind::Realloc), std::uint64_t{1});
}

LH_TEST(Mismatch, crossing_scalar_and_array_new_is_reported) {
    LH_CHECK_EQ(pairing(AllocationKind::New, ReleaseKind::DeleteArray), std::uint64_t{1});
    LH_CHECK_EQ(pairing(AllocationKind::NewArray, ReleaseKind::Delete), std::uint64_t{1});
}

LH_TEST(Mismatch, an_unobserved_half_is_never_a_finding) {
    // Old trace, or a block that was allocated before the agent went live.
    LH_CHECK_EQ(pairing(AllocationKind::New, ReleaseKind::Unknown), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::Unknown, ReleaseKind::Delete), std::uint64_t{0});
    LH_CHECK_EQ(pairing(AllocationKind::Unknown, ReleaseKind::Unknown), std::uint64_t{0});
}

LH_TEST(Mismatch, the_report_carries_enough_to_act_on) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0x9000, 8, 1, AllocationKind::New));
    freeIt(registry, 0x9000, ReleaseKind::Delete);

    registry.recordAllocation(makeAllocation(0x2000, 400, /*threadId=*/3,
                                             AllocationKind::NewArray));
    registry.recordDeallocation(0x2000, /*timestampNs=*/777, ReleaseKind::Free, /*threadId=*/5);

    const auto mismatches = registry.takeMismatchedFrees();
    LH_CHECK_EQ(mismatches.size(), std::size_t{1});
    LH_CHECK_EQ(mismatches[0].address, std::uint64_t{0x2000});
    LH_CHECK_EQ(mismatches[0].size, std::uint64_t{400});
    LH_CHECK_EQ(mismatches[0].timestampNs, std::uint64_t{777});
    LH_CHECK_EQ(mismatches[0].allocatedOnThread, std::uint64_t{3});
    LH_CHECK_EQ(mismatches[0].releasedOnThread, std::uint64_t{5});
    LH_CHECK(mismatches[0].allocatedBy == AllocationKind::NewArray);
    LH_CHECK(mismatches[0].releasedBy == ReleaseKind::Free);
    // The allocation stack has to survive the free that destroyed the record.
    LH_CHECK_EQ(mismatches[0].callStack.size(), std::size_t{2});
}

LH_TEST(Mismatch, a_mismatched_block_is_still_a_completed_free) {
    // The memory came back. It must not linger as a phantom leak.
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0x2000, 400, 1, AllocationKind::NewArray));

    LH_CHECK(freeIt(registry, 0x2000, ReleaseKind::Free));
    LH_CHECK_EQ(registry.liveCount(), std::size_t{0});
    LH_CHECK_EQ(registry.stats().totalBytesFreed, std::uint64_t{400});
}

LH_TEST(Mismatch, one_sided_interposition_suppresses_the_whole_class) {
    // `new` observed, `delete` never: our operator delete is not the one the
    // program calls. Every pairing we could derive would be an artefact.
    AllocationRegistry registry;
    for (std::uint64_t i = 1; i <= 20; ++i) {
        registry.recordAllocation(makeAllocation(i * 0x100, 32, 1, AllocationKind::New));
        freeIt(registry, i * 0x100, ReleaseKind::Free);
    }

    LH_CHECK(!registry.mismatchDetectionIsTrustworthy());
    LH_CHECK_EQ(registry.takeMismatchedFrees().size(), std::size_t{0});
    // And the counter is cleared too, because it is what drives the exit code.
    LH_CHECK_EQ(registry.stats().mismatchedFrees, std::uint64_t{0});
}

LH_TEST(Mismatch, a_pure_c_program_is_trusted_vacuously) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0x100, 32, 1, AllocationKind::Malloc));
    freeIt(registry, 0x100, ReleaseKind::Free);

    LH_CHECK(registry.mismatchDetectionIsTrustworthy());
}

LH_TEST(Mismatch, the_listing_is_capped_but_the_count_is_exact) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0x10, 8, 1, AllocationKind::New));
    freeIt(registry, 0x10, ReleaseKind::Delete);

    const std::uint64_t total = AllocationRegistry::kMaxRecordedMismatches + 500;
    for (std::uint64_t i = 1; i <= total; ++i) {
        registry.recordAllocation(makeAllocation(0x100000 + i * 0x40, 16, 1,
                                                 AllocationKind::NewArray));
        freeIt(registry, 0x100000 + i * 0x40, ReleaseKind::Free);
    }

    LH_CHECK_EQ(registry.stats().mismatchedFrees, total);
    LH_CHECK_EQ(registry.takeMismatchedFrees().size(),
                AllocationRegistry::kMaxRecordedMismatches);
}

// --- timeline events ------------------------------------------------------
//
// The registry is the only place that knows how big a freed block was, so it
// is where the deltas have to be produced. What matters here is that they stay
// balanced: a timeline that drifts from reality is a chart that lies.

LH_TEST(Registry, no_timeline_events_are_kept_unless_asked) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));
    freeIt(registry, 0xAAAA);

    LH_CHECK(registry.takeTimelineEvents().empty());
    LH_CHECK(!registry.timelineTruncated());
}

LH_TEST(Registry, an_allocation_and_its_free_cancel_out) {
    AllocationRegistry registry;
    registry.enableTimeline();
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));
    freeIt(registry, 0xAAAA);

    const auto events = registry.takeTimelineEvents();
    LH_CHECK_EQ(events.size(), std::size_t{2});

    std::int64_t sum = 0;
    for (const auto& event : events) {
        sum += event.deltaBytes;
    }
    LH_CHECK_EQ(sum, std::int64_t{0});
}

LH_TEST(Registry, a_reused_address_releases_the_block_it_evicts) {
    // An address reappearing while still live means we missed its free. The
    // registry drops the stale record, and the timeline has to see that as a
    // release -- otherwise a program that recycles addresses shows live memory
    // climbing forever while the real figure stays flat.
    AllocationRegistry registry;
    registry.enableTimeline();
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));
    registry.recordAllocation(makeAllocation(0xAAAA, 4096));  // same address, no free seen

    std::int64_t live = 0;
    for (const auto& event : registry.takeTimelineEvents()) {
        live += event.deltaBytes;
    }
    // 1024 in, 1024 back out, 4096 in: the block that is actually live.
    LH_CHECK_EQ(live, std::int64_t{4096});
}

LH_TEST(Registry, a_failed_allocation_contributes_no_event) {
    AllocationRegistry registry;
    registry.enableTimeline();
    registry.recordAllocation(makeAllocation(/*address=*/0, 1024));  // allocator returned null

    LH_CHECK(registry.takeTimelineEvents().empty());
}

LH_TEST(Registry, freeing_something_never_seen_contributes_no_event) {
    // Allocated before the agent was live -- by the dynamic loader, typically.
    // Subtracting bytes we never added would push the running total negative.
    AllocationRegistry registry;
    registry.enableTimeline();
    freeIt(registry, 0xDEAD);

    LH_CHECK(registry.takeTimelineEvents().empty());
}

LH_TEST(Registry, the_running_total_matches_the_live_bytes_the_registry_reports) {
    // The whole point: replaying the events must land on the same number the
    // registry arrived at independently.
    AllocationRegistry registry;
    registry.enableTimeline();

    for (std::uint64_t i = 0; i < 200; ++i) {
        registry.recordAllocation(makeAllocation(0x1000 + i * 0x100, 64 + i));
    }
    for (std::uint64_t i = 0; i < 150; ++i) {
        freeIt(registry, 0x1000 + i * 0x100);
    }

    std::int64_t live = 0;
    for (const auto& event : registry.takeTimelineEvents()) {
        live += event.deltaBytes;
    }

    std::uint64_t expected = 0;
    for (std::uint64_t i = 150; i < 200; ++i) {
        expected += 64 + i;
    }
    LH_CHECK_EQ(live, static_cast<std::int64_t>(expected));
    LH_CHECK_EQ(registry.liveCount(), std::size_t{50});
}

LH_TEST(Registry, the_timeline_stops_growing_at_the_cap_and_says_so) {
    // The cap is the one path a normal run never reaches, which makes it the
    // one most likely to be wrong. Reusing a single address keeps the live map
    // at one entry, so this exercises 2M events without 2M live allocations.
    AllocationRegistry registry;
    registry.enableTimeline();

    const std::size_t pairs = AllocationRegistry::kMaxTimelineEvents;  // 2 events each
    for (std::size_t i = 0; i < pairs; ++i) {
        registry.recordAllocation(makeAllocation(0xBEEF, 64));
        freeIt(registry, 0xBEEF);
    }

    LH_CHECK(registry.timelineTruncated());
    const auto events = registry.takeTimelineEvents();
    LH_CHECK_EQ(events.size(), AllocationRegistry::kMaxTimelineEvents);

    // Truncation must drop events, never corrupt the ones it kept.
    std::int64_t live = 0;
    for (const auto& event : events) {
        live += event.deltaBytes;
        LH_CHECK(live >= 0);
    }
}

// --- per-site allocation volume -------------------------------------------

LH_TEST(Registry, no_sites_are_tracked_unless_asked) {
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));
    LH_CHECK(registry.takeAllocationSites().empty());
}

LH_TEST(Registry, a_site_records_what_it_allocated_even_after_the_free) {
    // The whole point: the leak report forgets this block, the site does not.
    AllocationRegistry registry;
    registry.enableSiteTracking();
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));
    freeIt(registry, 0xAAAA);

    const auto sites = registry.takeAllocationSites();
    LH_CHECK_EQ(sites.size(), std::size_t{1});
    LH_CHECK_EQ(sites[0].totalBytes, std::uint64_t{1024});
    LH_CHECK_EQ(sites[0].count, std::uint64_t{1});
    LH_CHECK_EQ(sites[0].liveBytes, std::uint64_t{0});
    LH_CHECK_EQ(sites[0].liveCount, std::uint64_t{0});
    LH_CHECK_EQ(sites[0].peakLiveBytes, std::uint64_t{1024});
}

LH_TEST(Registry, a_sites_peak_is_the_most_it_held_at_once_not_its_total) {
    // Ten 1 KiB blocks, each freed before the next: 10 KiB of traffic through a
    // 1 KiB working set. Reporting 10 KiB as the peak would be wrong by 10x.
    AllocationRegistry registry;
    registry.enableSiteTracking();

    for (int i = 0; i < 10; ++i) {
        registry.recordAllocation(makeAllocation(0x5000, 1024));
        freeIt(registry, 0x5000);
    }

    const auto sites = registry.takeAllocationSites();
    LH_CHECK_EQ(sites.size(), std::size_t{1});
    LH_CHECK_EQ(sites[0].totalBytes, std::uint64_t{10 * 1024});
    LH_CHECK_EQ(sites[0].count, std::uint64_t{10});
    LH_CHECK_EQ(sites[0].peakLiveBytes, std::uint64_t{1024});
}

LH_TEST(Registry, holding_blocks_at_once_raises_the_sites_peak) {
    AllocationRegistry registry;
    registry.enableSiteTracking();

    for (int i = 0; i < 10; ++i) {
        registry.recordAllocation(makeAllocation(0x5000 + i * 0x100, 1024));
    }

    const auto sites = registry.takeAllocationSites();
    LH_CHECK_EQ(sites[0].peakLiveBytes, std::uint64_t{10 * 1024});
    LH_CHECK_EQ(sites[0].liveBytes, std::uint64_t{10 * 1024});
}

LH_TEST(Registry, different_stacks_are_different_sites) {
    AllocationRegistry registry;
    registry.enableSiteTracking();

    AllocationInfo first = makeAllocation(0x1000, 100);
    first.callStack = {0xAA, 0xBB};
    AllocationInfo second = makeAllocation(0x2000, 200);
    second.callStack = {0xAA, 0xCC};

    registry.recordAllocation(std::move(first));
    registry.recordAllocation(std::move(second));

    LH_CHECK_EQ(registry.takeAllocationSites().size(), std::size_t{2});
}

LH_TEST(Registry, freeing_a_block_from_an_untracked_site_invents_nothing) {
    // Tracking is switched on mid-stream here, so the free arrives for a site
    // that has no allocation on record. Creating a site from a free would put a
    // call site in the report that never allocated anything.
    AllocationRegistry registry;
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));
    registry.enableSiteTracking();
    freeIt(registry, 0xAAAA);

    LH_CHECK(registry.takeAllocationSites().empty());
}

LH_TEST(Registry, an_allocation_with_no_call_stack_is_not_a_site) {
    AllocationRegistry registry;
    registry.enableSiteTracking();

    AllocationInfo allocation = makeAllocation(0xAAAA, 1024);
    allocation.callStack.clear();
    registry.recordAllocation(std::move(allocation));

    LH_CHECK(registry.takeAllocationSites().empty());
}

LH_TEST(Registry, a_reused_address_discharges_the_site_it_evicts) {
    // The missed free has to reach the site totals too, or a program that
    // recycles addresses shows every site holding memory it released long ago.
    AllocationRegistry registry;
    registry.enableSiteTracking();
    registry.recordAllocation(makeAllocation(0xAAAA, 1024));
    registry.recordAllocation(makeAllocation(0xAAAA, 4096));  // same address, no free seen

    const auto sites = registry.takeAllocationSites();
    LH_CHECK_EQ(sites.size(), std::size_t{1});
    LH_CHECK_EQ(sites[0].totalBytes, std::uint64_t{1024 + 4096});
    LH_CHECK_EQ(sites[0].count, std::uint64_t{2});
    // Only the second block is live, and the peak never saw both at once.
    LH_CHECK_EQ(sites[0].liveBytes, std::uint64_t{4096});
    LH_CHECK_EQ(sites[0].liveCount, std::uint64_t{1});
    LH_CHECK_EQ(sites[0].peakLiveBytes, std::uint64_t{4096});
}

LH_TEST(Registry, site_tracking_stops_at_the_cap_and_says_so) {
    AllocationRegistry registry;
    registry.enableSiteTracking();

    for (std::uint64_t i = 0; i < AllocationRegistry::kMaxTrackedSites + 100; ++i) {
        AllocationInfo allocation = makeAllocation(0x1000 + i * 0x100, 64);
        allocation.callStack = {0xAA, i};  // a distinct stack every time
        registry.recordAllocation(std::move(allocation));
    }

    LH_CHECK(registry.siteTrackingTruncated());
    LH_CHECK_EQ(registry.takeAllocationSites().size(), AllocationRegistry::kMaxTrackedSites);
}
