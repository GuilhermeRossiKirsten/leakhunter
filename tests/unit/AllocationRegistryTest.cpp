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
