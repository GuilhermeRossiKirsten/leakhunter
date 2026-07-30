/// Attribution and grouping: the part of LeakHunter that turns raw addresses
/// into "this function leaked N bytes".

#include <map>
#include <string>

#include "TestFramework.hpp"
#include "leakhunter/analysis/LeakAnalyzer.hpp"

using leakhunter::AllocationInfo;
using leakhunter::AllocationKind;
using leakhunter::SessionStats;
using leakhunter::StackFrame;
using leakhunter::StackTrace;
using leakhunter::analysis::LeakAnalyzer;
using leakhunter::analysis::LeakReport;

namespace {

/// Stub resolver driven by a table, so tests describe stacks in terms of
/// function names instead of fabricated addresses.
class FakeResolver final : public leakhunter::symbols::ISymbolResolver {
public:
    void define(std::uint64_t address, std::string function, std::string module) {
        entries_[address] = {std::move(function), std::move(module)};
    }

    [[nodiscard]] StackFrame resolve(std::uint64_t address) const override {
        StackFrame frame;
        frame.address = address;
        frame.moduleBase = 0;

        if (const auto it = entries_.find(address); it != entries_.end()) {
            frame.function = it->second.first;
            frame.module = it->second.second;
            frame.resolved = true;
        }
        return frame;
    }

private:
    std::map<std::uint64_t, std::pair<std::string, std::string>> entries_;
};

AllocationInfo makeLeak(std::uint64_t address, std::uint64_t size,
                        std::vector<std::uint64_t> stack, std::uint64_t threadId = 1) {
    AllocationInfo allocation;
    allocation.address = address;
    allocation.size = size;
    allocation.threadId = threadId;
    allocation.kind = AllocationKind::Malloc;
    allocation.callStack = std::move(stack);
    return allocation;
}

constexpr std::uint64_t kMallocPc = 0x100;
constexpr std::uint64_t kOperatorNewPc = 0x110;
constexpr std::uint64_t kAgentPc = 0x120;
constexpr std::uint64_t kUserPc = 0x200;
constexpr std::uint64_t kOtherUserPc = 0x300;
constexpr std::uint64_t kMainPc = 0x400;

FakeResolver makeResolver() {
    FakeResolver resolver;
    resolver.define(kMallocPc, "malloc", "/usr/lib/libc.so.6");
    resolver.define(kOperatorNewPc, "operator new(unsigned long)", "/opt/libleakhunter_agent.so");
    resolver.define(kAgentPc, "leakhunter::agent::hook", "/opt/libleakhunter_agent.so");
    resolver.define(kUserPc, "allocateBuffer", "/home/dev/app");
    resolver.define(kOtherUserPc, "buildCache", "/home/dev/app");
    resolver.define(kMainPc, "main", "/home/dev/app");
    return resolver;
}

}  // namespace

LH_TEST(Analyzer, blames_the_first_frame_below_the_allocator) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report = analyzer.analyze(
        {makeLeak(0x1, 128, {kAgentPc, kMallocPc, kUserPc, kMainPc})}, /*mismatches=*/{},
        SessionStats{});

    LH_CHECK_EQ(report.leakCount, std::uint64_t{1});
    LH_CHECK_EQ(report.leaks.size(), std::size_t{1});

    const auto* responsible = report.leaks[0].responsible();
    LH_CHECK(responsible != nullptr);
    LH_CHECK_EQ(responsible->function, std::string{"allocateBuffer"});
}

LH_TEST(Analyzer, operator_new_is_never_the_culprit) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report = analyzer.analyze(
        {makeLeak(0x1, 64, {kOperatorNewPc, kOtherUserPc, kMainPc})}, /*mismatches=*/{},
        SessionStats{});

    const auto* responsible = report.leaks[0].responsible();
    LH_CHECK(responsible != nullptr);
    LH_CHECK_EQ(responsible->function, std::string{"buildCache"});
}

LH_TEST(Analyzer, groups_leaks_by_responsible_function) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    std::vector<AllocationInfo> live;
    for (std::uint64_t i = 0; i < 10; ++i) {
        live.push_back(makeLeak(0x1000 + i, 100, {kMallocPc, kUserPc, kMainPc}));
    }
    for (std::uint64_t i = 0; i < 3; ++i) {
        live.push_back(makeLeak(0x2000 + i, 50, {kMallocPc, kOtherUserPc, kMainPc}));
    }

    LeakReport report = analyzer.analyze(std::move(live), /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.groups.size(), std::size_t{2});
    LH_CHECK_EQ(report.leakCount, std::uint64_t{13});
    LH_CHECK_EQ(report.leakedBytes, std::uint64_t{10 * 100 + 3 * 50});

    // Sorted by bytes, so allocateBuffer (1000 B) comes before buildCache (150 B).
    LH_CHECK_EQ(report.groups[0].function, std::string{"allocateBuffer"});
    LH_CHECK_EQ(report.groups[0].count, std::uint64_t{10});
    LH_CHECK_EQ(report.groups[0].totalBytes, std::uint64_t{1000});
    LH_CHECK_EQ(report.groups[1].function, std::string{"buildCache"});
}

LH_TEST(Analyzer, counts_distinct_threads_per_group) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    std::vector<AllocationInfo> live;
    for (std::uint64_t thread = 1; thread <= 4; ++thread) {
        live.push_back(makeLeak(0x1000 + thread, 512, {kMallocPc, kUserPc}, thread));
    }

    LeakReport report = analyzer.analyze(std::move(live), /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.groups.size(), std::size_t{1});
    LH_CHECK_EQ(report.groups[0].threadCount, std::uint64_t{4});
}

LH_TEST(Analyzer, no_live_allocations_means_a_clean_report) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report = analyzer.analyze({}, /*mismatches=*/{}, SessionStats{});

    LH_CHECK(report.clean());
    LH_CHECK_EQ(report.leakCount, std::uint64_t{0});
    LH_CHECK(report.groups.empty());
}

LH_TEST(Analyzer, min_leak_size_filters_the_listing_but_not_the_totals) {
    const FakeResolver resolver = makeResolver();
    leakhunter::analysis::AnalyzerConfig config;
    config.minLeakSize = 100;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report = analyzer.analyze(
        {
            makeLeak(0x1, 500, {kMallocPc, kUserPc}),
            makeLeak(0x2, 10, {kMallocPc, kOtherUserPc}),
        },
        /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.leakCount, std::uint64_t{2});
    LH_CHECK_EQ(report.leakedBytes, std::uint64_t{510});
    LH_CHECK_EQ(report.leaks.size(), std::size_t{1});
    LH_CHECK_EQ(report.suppressedLeaks, std::uint64_t{1});
    LH_CHECK_EQ(report.suppressedBytes, std::uint64_t{10});
}

LH_TEST(Analyzer, leaks_are_sorted_largest_first) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report = analyzer.analyze(
        {
            makeLeak(0x1, 10, {kMallocPc, kUserPc}),
            makeLeak(0x2, 9000, {kMallocPc, kUserPc}),
            makeLeak(0x3, 300, {kMallocPc, kUserPc}),
        },
        /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.leaks[0].size, std::uint64_t{9000});
    LH_CHECK_EQ(report.leaks[1].size, std::uint64_t{300});
    LH_CHECK_EQ(report.leaks[2].size, std::uint64_t{10});
}

LH_TEST(Analyzer, unresolvable_stacks_still_produce_a_group) {
    // No symbols at all: the report must still show something actionable
    // instead of dropping the leak.
    const FakeResolver resolver;
    LeakAnalyzer analyzer(resolver);

    LeakReport report =
        analyzer.analyze({makeLeak(0x1, 64, {0xDEADBEEF})}, /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.leakCount, std::uint64_t{1});
    LH_CHECK_EQ(report.groups.size(), std::size_t{1});
    LH_CHECK_EQ(report.leaks[0].trace.size(), std::size_t{1});
    LH_CHECK(!report.leaks[0].trace[0].resolved);
}

LH_TEST(Frame, an_exact_name_is_shown_bare) {
    StackFrame frame;
    frame.address = 0x401234;
    frame.moduleBase = 0x400000;
    frame.symbolAddress = 0x401200;
    frame.function = "loadConfig";
    frame.module = "/home/dev/app";
    frame.file = "/home/dev/app/config.cpp";
    frame.line = 88;

    LH_CHECK(frame.preciseName());
    LH_CHECK_EQ(frame.displayName(), std::string{"loadConfig"});
}

LH_TEST(Frame, a_dladdr_name_carries_its_distance_from_the_symbol) {
    // No file:line, so the name came from a symbol table. The offset is what
    // tells the reader it may not be the containing function.
    StackFrame frame;
    frame.address = 0x401234;
    frame.moduleBase = 0x400000;
    frame.symbolAddress = 0x401200;
    frame.function = "someExportedThing";
    frame.module = "/bin/tool";

    LH_CHECK(!frame.preciseName());
    LH_CHECK_EQ(frame.symbolOffset(), std::uint64_t{0x34});
    LH_CHECK_EQ(frame.displayName(), std::string{"someExportedThing+0x34"});
}

LH_TEST(Frame, a_nameless_frame_shows_its_module_offset) {
    StackFrame frame;
    frame.address = 0x409049;
    frame.moduleBase = 0x400000;
    frame.module = "/bin/tool";

    LH_CHECK_EQ(frame.displayName(), std::string{"<unknown>+0x9049"});
}

LH_TEST(Analyzer, an_imprecise_name_is_located_by_module_offset) {
    // A stripped binary: the symbolizer answers with the nearest exported
    // symbol and no line number. The group must still point at an exact
    // address, otherwise the reader has nothing to act on.
    FakeResolver resolver;
    resolver.define(kUserPc, "nearestExportedSymbol", "/bin/stripped");

    LeakAnalyzer analyzer(resolver);
    LeakReport report =
        analyzer.analyze({makeLeak(0x1, 64, {kUserPc})}, /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.groups.size(), std::size_t{1});
    LH_CHECK_EQ(report.groups[0].location, std::string{"/bin/stripped+0x200"});
}

LH_TEST(Analyzer, allocator_only_stack_falls_back_to_the_innermost_frame) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report =
        analyzer.analyze({makeLeak(0x1, 64, {kMallocPc, kOperatorNewPc})}, /*mismatches=*/{},
                         SessionStats{});

    LH_CHECK_EQ(report.leaks[0].responsibleFrame, std::size_t{0});
    LH_CHECK_EQ(report.groups.size(), std::size_t{1});
}

// --- mismatched frees ------------------------------------------------------

namespace {

leakhunter::MismatchedFree makeMismatch(std::vector<std::uint64_t> callStack) {
    leakhunter::MismatchedFree mismatch;
    mismatch.address = 0x4000;
    mismatch.size = 128;
    mismatch.timestampNs = 5000;
    mismatch.allocatedBy = AllocationKind::NewArray;
    mismatch.releasedBy = leakhunter::ReleaseKind::Free;
    mismatch.callStack = std::move(callStack);
    return mismatch;
}

SessionStats statsWithMismatches(std::uint64_t count) {
    SessionStats stats;
    stats.mismatchedFrees = count;
    return stats;
}

}  // namespace

LH_TEST(Analyzer, a_mismatch_is_symbolised_and_blamed_like_a_leak) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report = analyzer.analyze(
        {}, {makeMismatch({kOperatorNewPc, kUserPc, kMainPc})}, statsWithMismatches(1));

    LH_CHECK_EQ(report.mismatchedFrees.size(), std::size_t{1});

    const auto* responsible = report.mismatchedFrees[0].responsible();
    LH_CHECK(responsible != nullptr);
    LH_CHECK_EQ(responsible->function, std::string{"allocateBuffer"});
}

LH_TEST(Analyzer, a_mismatch_alone_makes_the_run_dirty) {
    // No leaks at all, and the run still has to fail: the memory came back,
    // but the program has undefined behaviour in it.
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report =
        analyzer.analyze({}, {makeMismatch({kUserPc})}, statsWithMismatches(1));

    LH_CHECK_EQ(report.leakCount, std::uint64_t{0});
    LH_CHECK(!report.clean());
}

LH_TEST(Analyzer, opting_out_clears_the_finding_and_the_verdict) {
    const FakeResolver resolver = makeResolver();
    leakhunter::analysis::AnalyzerConfig config;
    config.detectMismatchedFrees = false;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report =
        analyzer.analyze({}, {makeMismatch({kUserPc})}, statsWithMismatches(1));

    LH_CHECK_EQ(report.mismatchedFrees.size(), std::size_t{0});
    // Opting out has to reach the exit code too, not just the listing.
    LH_CHECK_EQ(report.stats.mismatchedFrees, std::uint64_t{0});
    LH_CHECK(report.clean());
}

LH_TEST(Analyzer, mismatch_timestamps_are_relative_to_process_start) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    SessionStats stats = statsWithMismatches(1);
    stats.startTimestampNs = 1000;

    LeakReport report = analyzer.analyze({}, {makeMismatch({kUserPc})}, stats);
    LH_CHECK_EQ(report.mismatchedFrees[0].timestampNs, std::uint64_t{4000});
}

// --- suppressions ----------------------------------------------------------

namespace {

leakhunter::analysis::SuppressionSet suppressionsFrom(const std::string& text) {
    leakhunter::analysis::SuppressionSet set;
    LH_CHECK(set.loadText(text, "test.supp").hasValue());
    return set;
}

}  // namespace

LH_TEST(Analyzer, a_suppressed_leak_leaves_the_headline_counters_entirely) {
    // This is the difference between --suppressions and --min-leak-size: the
    // latter hides a leak from the listing but keeps it in the total, the
    // former stops it counting against you at all.
    const FakeResolver resolver = makeResolver();
    auto suppressions = suppressionsFrom("function:allocateBuffer\n");

    leakhunter::analysis::AnalyzerConfig config;
    config.suppressions = &suppressions;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report = analyzer.analyze(
        {
            makeLeak(0x1, 500, {kMallocPc, kUserPc}),        // suppressed
            makeLeak(0x2, 90, {kMallocPc, kOtherUserPc}),    // kept
        },
        /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.leakCount, std::uint64_t{1});
    LH_CHECK_EQ(report.leakedBytes, std::uint64_t{90});
    LH_CHECK_EQ(report.suppressedByRules, std::uint64_t{1});
    LH_CHECK_EQ(report.suppressedByRulesBytes, std::uint64_t{500});
    LH_CHECK_EQ(report.leaks.size(), std::size_t{1});
    LH_CHECK_EQ(report.groups.size(), std::size_t{1});
}

LH_TEST(Analyzer, suppressing_everything_makes_the_run_clean) {
    const FakeResolver resolver = makeResolver();
    auto suppressions = suppressionsFrom("stack:*\n");

    leakhunter::analysis::AnalyzerConfig config;
    config.suppressions = &suppressions;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report =
        analyzer.analyze({makeLeak(0x1, 500, {kMallocPc, kUserPc})},
                         /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.leakCount, std::uint64_t{0});
    LH_CHECK(report.clean());
    LH_CHECK_EQ(report.suppressedByRules, std::uint64_t{1});
}

LH_TEST(Analyzer, rule_hits_are_attributed_and_sorted_by_bytes) {
    const FakeResolver resolver = makeResolver();
    auto suppressions = suppressionsFrom("function:allocateBuffer\nfunction:buildCache\n");

    leakhunter::analysis::AnalyzerConfig config;
    config.suppressions = &suppressions;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report = analyzer.analyze(
        {
            makeLeak(0x1, 100, {kMallocPc, kUserPc}),          // rule 0
            makeLeak(0x2, 50, {kMallocPc, kUserPc}),           // rule 0
            makeLeak(0x3, 9000, {kOperatorNewPc, kOtherUserPc}),  // rule 1
        },
        /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.suppressedByRules, std::uint64_t{3});
    LH_CHECK_EQ(report.suppressedByRulesBytes, std::uint64_t{9150});
    LH_CHECK_EQ(report.ruleHits.size(), std::size_t{2});

    // Biggest suppression first: that is the rule worth re-examining.
    LH_CHECK_EQ(report.ruleHits[0].bytes, std::uint64_t{9000});
    LH_CHECK_EQ(report.ruleHits[0].count, std::uint64_t{1});
    LH_CHECK(report.ruleHits[0].rule.find("buildCache") != std::string::npos);
    LH_CHECK_EQ(report.ruleHits[1].bytes, std::uint64_t{150});
    LH_CHECK_EQ(report.ruleHits[1].count, std::uint64_t{2});
}

LH_TEST(Analyzer, a_rule_that_matched_nothing_is_reported) {
    const FakeResolver resolver = makeResolver();
    auto suppressions = suppressionsFrom("function:allocateBuffer\nfunction:renamedLastYear\n");

    leakhunter::analysis::AnalyzerConfig config;
    config.suppressions = &suppressions;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report =
        analyzer.analyze({makeLeak(0x1, 100, {kMallocPc, kUserPc})},
                         /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.unusedRules.size(), std::size_t{1});
    LH_CHECK(report.unusedRules[0].find("renamedLastYear") != std::string::npos);
    // And it must carry its location so the user can go delete it.
    LH_CHECK(report.unusedRules[0].find("test.supp:2") != std::string::npos);
}

LH_TEST(Analyzer, a_suppression_outranks_the_runtime_classification) {
    // A user rule is more specific than our own libc heuristic, so it wins and
    // the leak lands in the suppression bucket, not the runtime one.
    FakeResolver resolver;
    resolver.define(kUserPc, "someLibcThing", "/lib/x86_64-linux-gnu/libc.so.6");

    auto suppressions = suppressionsFrom("module:*libc.so*\n");
    leakhunter::analysis::AnalyzerConfig config;
    config.suppressions = &suppressions;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report = analyzer.analyze({makeLeak(0x1, 64, {kUserPc})},
                                        /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.suppressedByRules, std::uint64_t{1});
    LH_CHECK_EQ(report.runtimeLeakCount, std::uint64_t{0});
    LH_CHECK_EQ(report.leakCount, std::uint64_t{0});
}

LH_TEST(Analyzer, no_suppressions_configured_changes_nothing) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report =
        analyzer.analyze({makeLeak(0x1, 500, {kMallocPc, kUserPc})},
                         /*mismatches=*/{}, SessionStats{});

    LH_CHECK_EQ(report.leakCount, std::uint64_t{1});
    LH_CHECK_EQ(report.suppressedByRules, std::uint64_t{0});
    LH_CHECK_EQ(report.ruleHits.size(), std::size_t{0});
    LH_CHECK_EQ(report.unusedRules.size(), std::size_t{0});
}

LH_TEST(Analyzer, suppressions_cover_mismatched_frees_too) {
    // Vendored code with UB you cannot fix has to be acceptable, and a rule
    // that applied to leaks but silently not to mismatches would be a trap:
    // the user writes rules, still gets exit 1, and nothing explains why.
    const FakeResolver resolver = makeResolver();
    auto suppressions = suppressionsFrom("function:allocateBuffer\n");

    leakhunter::analysis::AnalyzerConfig config;
    config.suppressions = &suppressions;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report = analyzer.analyze({}, {makeMismatch({kOperatorNewPc, kUserPc})},
                                         statsWithMismatches(1));

    LH_CHECK_EQ(report.mismatchedFrees.size(), std::size_t{0});
    LH_CHECK_EQ(report.suppressedMismatchesByRules, std::uint64_t{1});
    LH_CHECK_EQ(report.stats.mismatchedFrees, std::uint64_t{0});
    LH_CHECK_EQ(report.suppressedMismatches, std::uint64_t{0});
    LH_CHECK(report.clean());

    // The rule has to be credited, or the per-rule accounting lies.
    LH_CHECK_EQ(report.ruleHits.size(), std::size_t{1});
    LH_CHECK_EQ(report.ruleHits[0].count, std::uint64_t{1});
    LH_CHECK_EQ(report.unusedRules.size(), std::size_t{0});

    // ...but its bytes stay zero. The block was returned, so counting its size
    // as suppressed memory would overstate what the rule is hiding.
    LH_CHECK_EQ(report.ruleHits[0].bytes, std::uint64_t{0});
    LH_CHECK_EQ(report.suppressedByRulesBytes, std::uint64_t{0});
}

LH_TEST(Analyzer, a_rule_that_misses_leaves_the_mismatch_standing) {
    const FakeResolver resolver = makeResolver();
    auto suppressions = suppressionsFrom("function:somethingElse\n");

    leakhunter::analysis::AnalyzerConfig config;
    config.suppressions = &suppressions;

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report = analyzer.analyze({}, {makeMismatch({kOperatorNewPc, kUserPc})},
                                         statsWithMismatches(1));

    LH_CHECK_EQ(report.mismatchedFrees.size(), std::size_t{1});
    LH_CHECK_EQ(report.stats.mismatchedFrees, std::uint64_t{1});
    LH_CHECK(!report.clean());
}

LH_TEST(Analyzer, mismatches_beyond_the_cap_are_counted_not_listed) {
    const FakeResolver resolver = makeResolver();
    leakhunter::analysis::AnalyzerConfig config;
    config.maxDetailedMismatches = 3;

    std::vector<leakhunter::MismatchedFree> mismatches;
    for (int i = 0; i < 10; ++i) {
        mismatches.push_back(makeMismatch({kUserPc}));
    }

    LeakAnalyzer analyzer(resolver, config);
    LeakReport report = analyzer.analyze({}, std::move(mismatches), statsWithMismatches(10));

    LH_CHECK_EQ(report.mismatchedFrees.size(), std::size_t{3});
    LH_CHECK_EQ(report.suppressedMismatches, std::uint64_t{7});
}

// --- hot spots ------------------------------------------------------------
//
// Not leaks. A site here may have returned every byte it took, and the whole
// point is that the leak report cannot see it.

namespace {

leakhunter::AllocationSite site(std::vector<std::uint64_t> stack, std::uint64_t totalBytes,
                                std::uint64_t count, std::uint64_t peakLive,
                                std::uint64_t liveBytes = 0, std::uint64_t liveCount = 0) {
    leakhunter::AllocationSite result;
    result.callStack = std::move(stack);
    result.totalBytes = totalBytes;
    result.count = count;
    result.peakLiveBytes = peakLive;
    result.liveBytes = liveBytes;
    result.liveCount = liveCount;
    return result;
}

}  // namespace

LH_TEST(HotSpots, a_site_that_freed_everything_is_still_reported) {
    // The reason this feature exists. 4 MiB through one function, nothing
    // leaked; the leak report is empty and correct, and silent about the cost.
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report, {site({kMallocPc, kUserPc, kMainPc}, 4 * 1024 * 1024, 4096, 1024)});

    LH_CHECK_EQ(report.hotSpots.size(), std::size_t{1});
    LH_CHECK_EQ(report.hotSpots[0].function, std::string{"allocateBuffer"});
    LH_CHECK_EQ(report.hotSpots[0].totalBytes, std::uint64_t{4 * 1024 * 1024});
    LH_CHECK_EQ(report.hotSpots[0].liveBytes, std::uint64_t{0});
    LH_CHECK_EQ(report.hotSpots[0].averageBytes(), std::uint64_t{1024});
    // 4 MiB through a 1 KiB working set.
    LH_CHECK(report.hotSpots[0].turnover() > 4095.0);
}

LH_TEST(HotSpots, one_function_reached_by_two_paths_is_one_row) {
    // The registry keys on the whole stack because it has no symbols. Left
    // unmerged, a helper called from three places is three rows with identical
    // names and no way to tell them apart.
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report, {
        site({kMallocPc, kUserPc, kMainPc}, 1000, 10, 100),
        site({kMallocPc, kUserPc, kOtherUserPc, kMainPc}, 500, 5, 50),
    });

    LH_CHECK_EQ(report.hotSpots.size(), std::size_t{1});
    LH_CHECK_EQ(report.hotSpots[0].function, std::string{"allocateBuffer"});
    LH_CHECK_EQ(report.hotSpots[0].totalBytes, std::uint64_t{1500});
    LH_CHECK_EQ(report.hotSpots[0].count, std::uint64_t{15});
    // Summed: both paths can hold their blocks at the same moment, so taking
    // the max would claim the site held less than it demonstrably did.
    LH_CHECK_EQ(report.hotSpots[0].peakLiveBytes, std::uint64_t{150});
}

LH_TEST(HotSpots, distinct_functions_stay_distinct) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report, {
        site({kMallocPc, kUserPc, kMainPc}, 1000, 10, 100),
        site({kMallocPc, kOtherUserPc, kMainPc}, 500, 5, 50),
    });

    LH_CHECK_EQ(report.hotSpots.size(), std::size_t{2});
    LH_CHECK_EQ(report.hotSpots[0].function, std::string{"allocateBuffer"});
    LH_CHECK_EQ(report.hotSpots[1].function, std::string{"buildCache"});
}

LH_TEST(HotSpots, the_ranking_is_by_bytes_moved_not_bytes_kept) {
    // A site that churns 1 MiB and keeps nothing outranks one that keeps 4 KiB
    // and never allocated again. That is the opposite of the leak ordering, on
    // purpose: this section answers a different question.
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report, {
        site({kMallocPc, kOtherUserPc, kMainPc}, 4096, 1, 4096, 4096, 1),
        site({kMallocPc, kUserPc, kMainPc}, 1024 * 1024, 1024, 1024),
    });

    LH_CHECK_EQ(report.hotSpots[0].function, std::string{"allocateBuffer"});
    LH_CHECK_EQ(report.hotSpots[0].liveBytes, std::uint64_t{0});
    LH_CHECK_EQ(report.hotSpots[1].function, std::string{"buildCache"});
}

LH_TEST(HotSpots, the_keep_limit_is_respected_and_keeps_the_largest) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report,
                          {
                              site({kMallocPc, kUserPc, kMainPc}, 100, 1, 100),
                              site({kMallocPc, kOtherUserPc, kMainPc}, 999, 1, 999),
                              site({kMallocPc, kMainPc}, 500, 1, 500),
                          },
                          /*keep=*/1);

    LH_CHECK_EQ(report.hotSpots.size(), std::size_t{1});
    LH_CHECK_EQ(report.hotSpots[0].totalBytes, std::uint64_t{999});
}

LH_TEST(HotSpots, no_sites_produces_no_section) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report, {});
    LH_CHECK(report.hotSpots.empty());
}

LH_TEST(HotSpots, an_all_allocator_stack_still_yields_a_row) {
    // Every frame looks like infrastructure. findResponsibleFrame falls back to
    // frame 0 rather than dropping the site, because "we cannot name it" is not
    // a reason to hide a megabyte of traffic.
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report, {site({kMallocPc, kOperatorNewPc}, 4096, 4, 1024)});
    LH_CHECK_EQ(report.hotSpots.size(), std::size_t{1});
    LH_CHECK_EQ(report.hotSpots[0].totalBytes, std::uint64_t{4096});
}

LH_TEST(HotSpots, turnover_is_zero_rather_than_infinite_when_nothing_was_held) {
    const FakeResolver resolver = makeResolver();
    LeakAnalyzer analyzer(resolver);

    LeakReport report;
    analyzer.rankHotSpots(report, {site({kMallocPc, kUserPc}, 1024, 1, /*peakLive=*/0)});
    LH_CHECK_EQ(report.hotSpots[0].turnover(), 0.0);
    LH_CHECK_EQ(report.hotSpots[0].averageBytes(), std::uint64_t{1024});
}
