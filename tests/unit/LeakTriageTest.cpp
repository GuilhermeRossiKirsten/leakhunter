/// Turning timings into a verdict.
///
/// The value of triage is that someone acts on it, so a wrong classification is
/// worse than none: it sends people to fix the wrong site. Most of these tests
/// are about refusing to claim more than the data supports.

#include <string>

#include "TestFramework.hpp"
#include "leakhunter/analysis/LeakReport.hpp"
#include "leakhunter/analysis/LeakTriage.hpp"
#include "leakhunter/analysis/SuppressionSet.hpp"

using leakhunter::analysis::LeakGroup;
using leakhunter::analysis::LeakPattern;
using leakhunter::analysis::LeakReport;
using leakhunter::analysis::SuppressionSet;
using leakhunter::analysis::triageLeaks;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000ULL;

leakhunter::StackFrame frame(std::string function, std::string file = "/app/src/a.cpp",
                             std::uint32_t line = 10) {
    leakhunter::StackFrame result;
    result.function = std::move(function);
    result.module = "/app/bin/app";
    result.file = std::move(file);
    result.line = line;  // file + line => preciseName()
    result.resolved = true;
    return result;
}

/// A report with one site whose leaks land at the given times.
LeakReport reportWith(const std::vector<std::uint64_t>& timesNs, std::uint64_t runNs,
                      std::uint64_t bytesEach = 1024,
                      leakhunter::AllocationKind kind = leakhunter::AllocationKind::Malloc,
                      std::string function = "doWork") {
    LeakReport report;
    report.stats.startTimestampNs = 0;
    report.stats.endTimestampNs = runNs;

    LeakGroup group;
    group.function = function;
    group.representativeTrace = {frame(function)};
    group.blamedFrame = 0;
    group.threadCount = 1;

    for (const std::uint64_t when : timesNs) {
        leakhunter::analysis::Leak leak;
        leak.size = bytesEach;
        leak.timestampNs = when;
        leak.kind = kind;
        leak.trace = group.representativeTrace;
        group.leakIndices.push_back(report.leaks.size());
        report.leaks.push_back(leak);
    }
    group.count = timesNs.size();
    group.totalBytes = bytesEach * timesNs.size();

    report.groups.push_back(std::move(group));
    return report;
}

}  // namespace

LH_TEST(Triage, a_site_spread_across_the_run_grows_with_uptime) {
    // 100 blocks of 1 KiB evenly across an hour.
    std::vector<std::uint64_t> times;
    for (int i = 0; i < 100; ++i) {
        times.push_back(static_cast<std::uint64_t>(i) * 36 * kSecond);
    }
    LeakReport report = reportWith(times, 3600 * kSecond);
    triageLeaks(report);

    const auto& triage = report.groups[0].triage;
    LH_CHECK(triage.pattern == LeakPattern::Steady);

    // 100 KiB over ~0.99 h. Allow slack: the window is first..last, not the
    // whole run, so the rate is slightly above the naive figure.
    LH_CHECK(triage.bytesPerHour > 100000.0);
    LH_CHECK(triage.bytesPerHour < 120000.0);
    LH_CHECK(!triage.advice.empty());
}

LH_TEST(Triage, a_site_that_finishes_early_is_a_fixed_cost) {
    LeakReport report = reportWith({0, kSecond, 2 * kSecond}, 3600 * kSecond);
    triageLeaks(report);

    LH_CHECK(report.groups[0].triage.pattern == LeakPattern::Startup);
    // A fixed cost has no meaningful rate, and inventing one invites a graph.
    LH_CHECK_EQ(report.groups[0].triage.bytesPerHour, 0.0);
}

LH_TEST(Triage, a_cluster_in_the_middle_is_a_burst) {
    const std::uint64_t half = 1800 * kSecond;
    LeakReport report = reportWith({half, half + kSecond, half + 2 * kSecond}, 3600 * kSecond);
    triageLeaks(report);
    LH_CHECK(report.groups[0].triage.pattern == LeakPattern::Burst);
}

LH_TEST(Triage, a_single_block_is_one_shot) {
    LeakReport report = reportWith({1800 * kSecond}, 3600 * kSecond);
    triageLeaks(report);
    LH_CHECK(report.groups[0].triage.pattern == LeakPattern::OneShot);
}

LH_TEST(Triage, a_run_too_short_to_classify_says_so) {
    // Five milliseconds. Every site looks clustered because the whole run is a
    // cluster; claiming "burst" there would be inventing information.
    LeakReport report = reportWith({0, 1'000'000, 2'000'000}, 5'000'000);
    triageLeaks(report);

    const auto& triage = report.groups[0].triage;
    LH_CHECK(triage.pattern == LeakPattern::Unknown);
    LH_CHECK(!triage.advice.empty());
    LH_CHECK(triage.advice.front().find("too short") != std::string::npos);
}

LH_TEST(Triage, the_wall_clock_covers_for_a_missing_end_marker) {
    // A target we stopped writes no end record, so stats.durationNs() is 0 --
    // and that is exactly the long-running service, where growth matters most.
    std::vector<std::uint64_t> times;
    for (int i = 0; i < 20; ++i) {
        times.push_back(static_cast<std::uint64_t>(i) * kSecond);
    }
    LeakReport report = reportWith(times, /*runNs=*/0);
    report.stats.endTimestampNs = 0;      // no end marker
    report.process.durationMs = 20'000;   // ...but the host timed it

    triageLeaks(report);
    LH_CHECK(report.groups[0].triage.pattern == LeakPattern::Steady);
    LH_CHECK(report.groups[0].triage.bytesPerHour > 0.0);
}

LH_TEST(Triage, no_duration_at_all_produces_no_rate) {
    LeakReport report = reportWith({0, kSecond, 2 * kSecond}, /*runNs=*/0);
    report.stats.endTimestampNs = 0;
    report.process.durationMs = 0;

    triageLeaks(report);
    // No division by zero, no absurd extrapolation.
    LH_CHECK_EQ(report.groups[0].triage.bytesPerHour, 0.0);
    LH_CHECK(report.groups[0].triage.pattern == LeakPattern::Unknown);
}

LH_TEST(Triage, a_partial_sample_is_flagged_and_still_uses_the_full_byte_count) {
    // --min-leak-size and the detail cap both list fewer leaks than the site
    // has. The window comes from the sample; the bytes must not.
    std::vector<std::uint64_t> times;
    for (int i = 0; i < 10; ++i) {
        times.push_back(static_cast<std::uint64_t>(i) * 360 * kSecond);
    }
    LeakReport report = reportWith(times, 3600 * kSecond);
    report.groups[0].count = 1000;              // the site really leaked 1000
    report.groups[0].totalBytes = 1024 * 1000;  // ...and this many bytes

    triageLeaks(report);
    const auto& triage = report.groups[0].triage;
    LH_CHECK(triage.sampleIsPartial);
    // The rate has to reflect all 1000, not the 10 that were listed.
    LH_CHECK(triage.bytesPerHour > 1'000'000.0);
}

LH_TEST(Triage, a_group_with_no_listed_leaks_gets_no_verdict) {
    LeakReport report;
    LeakGroup group;
    group.function = "mystery";
    group.count = 5;
    group.totalBytes = 500;
    report.groups.push_back(group);

    triageLeaks(report);
    LH_CHECK(report.groups[0].triage.empty());
}

LH_TEST(Triage, a_clean_report_produces_nothing) {
    LeakReport report;
    triageLeaks(report);  // must not crash on an empty report
    LH_CHECK(report.groups.empty());
}

// --- the suppression rule -------------------------------------------------

LH_TEST(Triage, the_suggested_rule_actually_matches_the_site_it_came_from) {
    // The test that matters. A rule that reads well and matches nothing is
    // worse than no rule: the user pastes it, the leak keeps being reported,
    // and they conclude suppressions are broken.
    LeakReport report = reportWith({0, kSecond}, 3600 * kSecond, 1024,
                                   leakhunter::AllocationKind::Malloc,
                                   "poc::(anonymous namespace)::indexBatch(unsigned long)");
    triageLeaks(report);

    const std::string rule = report.groups[0].triage.suppressionRule;
    LH_CHECK(!rule.empty());

    SuppressionSet suppressions;
    LH_CHECK(suppressions.loadText(rule + "\n", "suggested").hasValue());
    LH_CHECK_EQ(suppressions.match(report.groups[0].representativeTrace, 0), std::size_t{0});
}

LH_TEST(Triage, the_rule_drops_the_parameter_list) {
    // A signature in a suppression file is unreadable and brittle: it stops
    // matching the day someone adds a parameter.
    LeakReport report = reportWith({0, kSecond}, 3600 * kSecond, 1024,
                                   leakhunter::AllocationKind::New, "ns::build(int, char const*)");
    triageLeaks(report);

    const std::string rule = report.groups[0].triage.suppressionRule;
    LH_CHECK_EQ(rule, std::string{"function:*ns::build*"});
    LH_CHECK(rule.find("int") == std::string::npos);
}

LH_TEST(Triage, an_imprecise_name_falls_back_to_file_or_module) {
    LeakReport report = reportWith({0, kSecond}, 3600 * kSecond);
    // No file/line => preciseName() is false => the name is dladdr's nearest
    // exported symbol, which must not be turned into a function: rule.
    report.groups[0].representativeTrace[0].file.clear();
    report.groups[0].representativeTrace[0].line = 0;

    triageLeaks(report);
    const std::string rule = report.groups[0].triage.suppressionRule;
    LH_CHECK(rule.rfind("module:", 0) == 0);
}

LH_TEST(Triage, the_advice_names_an_owning_type_for_the_allocator_used) {
    for (const auto& [kind, expected] :
         {std::pair{leakhunter::AllocationKind::New, "unique_ptr"},
          std::pair{leakhunter::AllocationKind::NewArray, "vector"},
          std::pair{leakhunter::AllocationKind::Malloc, "vector"}}) {
        LeakReport report = reportWith({0, kSecond}, 3600 * kSecond, 1024, kind);
        triageLeaks(report);

        bool mentioned = false;
        for (const std::string& line : report.groups[0].triage.advice) {
            mentioned = mentioned || line.find(expected) != std::string::npos;
        }
        LH_CHECK(mentioned);
    }
}

LH_TEST(Triage, a_multithreaded_site_says_the_fix_must_hold_under_concurrency) {
    LeakReport report = reportWith({0, kSecond}, 3600 * kSecond);
    report.groups[0].threadCount = 8;

    triageLeaks(report);
    bool mentioned = false;
    for (const std::string& line : report.groups[0].triage.advice) {
        mentioned = mentioned || line.find("8 threads") != std::string::npos;
    }
    LH_CHECK(mentioned);
}
