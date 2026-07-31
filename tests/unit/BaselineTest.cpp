/// Gating on the delta rather than the absolute count.
///
/// This is the part that decides whether a build goes red, so the tests are
/// mostly about not failing a build for the wrong reason -- which is the failure
/// mode that gets a tool removed from a pipeline.

#include <fstream>
#include <string>

#include "TestFramework.hpp"
#include "leakhunter/analysis/Baseline.hpp"
#include "leakhunter/analysis/LeakReport.hpp"
#include "leakhunter/core/ScopedTempFile.hpp"

using leakhunter::analysis::compareToBaseline;
using leakhunter::analysis::LeakGroup;
using leakhunter::analysis::LeakReport;

namespace {

/// Writes a minimal report.json with the given sites.
std::string writeBaseline(const std::string& groupsJson, std::uint64_t mismatches = 0) {
    static int counter = 0;
    const std::string path =
        "/tmp/leakhunter-baseline-test-" + std::to_string(++counter) + ".json";
    std::ofstream out(path);
    out << R"({"run":{"generatedAt":"2026-01-01T00:00:00Z"},"summary":{"mismatchedFreeCount":)"
        << mismatches << R"(},"groups":[)" << groupsJson << "]}";
    return path;
}

std::string site(const std::string& function, std::uint64_t bytes, std::uint64_t count) {
    return R"({"function":")" + function + R"(","module":"/app","totalBytes":)" +
           std::to_string(bytes) + R"(,"count":)" + std::to_string(count) +
           R"(,"location":"a.cpp:1"})";
}

LeakReport reportWith(std::vector<std::tuple<std::string, std::uint64_t, std::uint64_t>> sites,
                      std::uint64_t mismatches = 0) {
    LeakReport report;
    for (auto& [function, bytes, count] : sites) {
        LeakGroup group;
        group.function = function;
        group.module = "/app";
        group.totalBytes = bytes;
        group.count = count;
        report.groups.push_back(std::move(group));
    }
    report.stats.mismatchedFrees = mismatches;
    return report;
}

}  // namespace

LH_TEST(Baseline, an_unchanged_run_holds_the_line) {
    // The case that makes the tool adoptable: a codebase with existing leaks
    // must not fail every build forever.
    const std::string path = writeBaseline(site("leakA", 1024, 10));
    const LeakReport report = reportWith({{"leakA", 1024, 10}});

    auto diff = compareToBaseline(report, path);
    LH_CHECK(diff.hasValue());
    LH_CHECK(!diff.value().regressed());
    LH_CHECK_EQ(diff.value().unchangedSites, std::uint64_t{1});
    LH_CHECK(diff.value().newSites.empty());
}

LH_TEST(Baseline, a_site_the_baseline_never_had_is_a_regression) {
    const std::string path = writeBaseline(site("leakA", 1024, 10));
    const LeakReport report = reportWith({{"leakA", 1024, 10}, {"leakB", 512, 5}});

    auto diff = compareToBaseline(report, path);
    LH_CHECK(diff.value().regressed());
    LH_CHECK_EQ(diff.value().newSites.size(), std::size_t{1});
    LH_CHECK_EQ(diff.value().newSites[0].function, std::string{"leakB"});
}

LH_TEST(Baseline, an_existing_site_that_leaks_more_is_a_regression) {
    const std::string path = writeBaseline(site("leakA", 1024, 10));
    const LeakReport report = reportWith({{"leakA", 2048, 20}});

    auto diff = compareToBaseline(report, path);
    LH_CHECK(diff.value().regressed());
    LH_CHECK_EQ(diff.value().worseSites.size(), std::size_t{1});
    LH_CHECK_EQ(diff.value().worseSites[0].byteDelta(), std::int64_t{1024});
}

LH_TEST(Baseline, leaking_less_at_a_known_site_is_not_a_regression) {
    // Partial progress must not read as failure, or nobody makes partial
    // progress.
    const std::string path = writeBaseline(site("leakA", 1024, 10));
    const LeakReport report = reportWith({{"leakA", 512, 5}});

    auto diff = compareToBaseline(report, path);
    LH_CHECK(!diff.value().regressed());
}

LH_TEST(Baseline, a_site_that_disappeared_is_reported_as_fixed) {
    const std::string path = writeBaseline(site("leakA", 1024, 10) + "," + site("leakB", 512, 5));
    const LeakReport report = reportWith({{"leakA", 1024, 10}});

    auto diff = compareToBaseline(report, path);
    LH_CHECK(!diff.value().regressed());
    LH_CHECK(diff.value().improved());
    LH_CHECK_EQ(diff.value().fixedSites.size(), std::size_t{1});
    LH_CHECK_EQ(diff.value().fixedSites[0].function, std::string{"leakB"});
}

LH_TEST(Baseline, a_new_mismatched_free_is_a_regression_even_with_no_new_leaks) {
    // Undefined behaviour must not slip in under an unchanged leak count.
    const std::string path = writeBaseline(site("leakA", 1024, 10), /*mismatches=*/2);
    const LeakReport report = reportWith({{"leakA", 1024, 10}}, /*mismatches=*/3);

    auto diff = compareToBaseline(report, path);
    LH_CHECK(diff.value().regressed());
}

LH_TEST(Baseline, fixing_a_mismatched_free_counts_as_improvement) {
    const std::string path = writeBaseline(site("leakA", 1024, 10), /*mismatches=*/2);
    const LeakReport report = reportWith({{"leakA", 1024, 10}}, /*mismatches=*/0);

    auto diff = compareToBaseline(report, path);
    LH_CHECK(!diff.value().regressed());
    LH_CHECK(diff.value().improved());
}

LH_TEST(Baseline, a_missing_file_is_an_error_not_an_empty_baseline) {
    // The important one. Treating a mistyped path as "no previous findings"
    // would turn every pre-existing leak into a new one and fail the build for
    // a reason that has nothing to do with the code.
    const LeakReport report = reportWith({{"leakA", 1024, 10}});
    auto diff = compareToBaseline(report, "/tmp/leakhunter-no-such-baseline.json");
    LH_CHECK(!diff.hasValue());
}

LH_TEST(Baseline, a_file_that_is_not_a_report_is_rejected) {
    const std::string path = "/tmp/leakhunter-baseline-notareport.json";
    std::ofstream(path) << R"({"hello":"world"})";

    const LeakReport report = reportWith({{"leakA", 1024, 10}});
    auto diff = compareToBaseline(report, path);
    LH_CHECK(!diff.hasValue());
}

LH_TEST(Baseline, malformed_json_is_rejected_rather_than_ignored) {
    const std::string path = "/tmp/leakhunter-baseline-broken.json";
    std::ofstream(path) << "{ this is not json";

    const LeakReport report = reportWith({{"leakA", 1024, 10}});
    auto diff = compareToBaseline(report, path);
    LH_CHECK(!diff.hasValue());
}

LH_TEST(Baseline, the_same_function_in_another_module_is_a_different_site) {
    // Static functions share names across translation units; collapsing them
    // would hide a genuinely new leak behind an unrelated known one.
    const std::string path = writeBaseline(
        R"({"function":"helper","module":"/app/a.so","totalBytes":1024,"count":10})");

    LeakReport report;
    LeakGroup group;
    group.function = "helper";
    group.module = "/app/b.so";  // different object
    group.totalBytes = 1024;
    group.count = 10;
    report.groups.push_back(std::move(group));

    auto diff = compareToBaseline(report, path);
    LH_CHECK(diff.value().regressed());
    LH_CHECK_EQ(diff.value().newSites.size(), std::size_t{1});
}

LH_TEST(Baseline, an_empty_baseline_makes_every_site_new) {
    // A baseline recorded on genuinely clean code: anything appearing later is
    // by definition new, and must fail.
    const std::string path = writeBaseline("");
    const LeakReport report = reportWith({{"leakA", 1024, 10}});

    auto diff = compareToBaseline(report, path);
    LH_CHECK(diff.value().regressed());
    LH_CHECK_EQ(diff.value().newSites.size(), std::size_t{1});
}

// --- tolerance ------------------------------------------------------------
//
// A gate that fires at random is worse than no gate: the team learns to re-run
// the build until it passes, and then it is decoration.

LH_TEST(Baseline, growth_inside_the_tolerance_is_not_a_regression) {
    const std::string path = writeBaseline(site("leakA", 1024, 100));
    const LeakReport report = reportWith({{"leakA", 1030, 103}});  // +3%

    auto diff = compareToBaseline(report, path, /*tolerancePercent=*/5.0);
    LH_CHECK(!diff.value().regressed());
    LH_CHECK(diff.value().worseSites.empty());
    // ...and it is listed, so the allowance is auditable rather than silent.
    LH_CHECK_EQ(diff.value().withinTolerance.size(), std::size_t{1});
}

LH_TEST(Baseline, growth_beyond_the_tolerance_still_fails) {
    const std::string path = writeBaseline(site("leakA", 1024, 100));
    const LeakReport report = reportWith({{"leakA", 1200, 120}});  // +20%

    auto diff = compareToBaseline(report, path, /*tolerancePercent=*/5.0);
    LH_CHECK(diff.value().regressed());
    LH_CHECK_EQ(diff.value().worseSites.size(), std::size_t{1});
}

LH_TEST(Baseline, tolerance_never_admits_a_brand_new_site) {
    // The line that keeps tolerance honest. It is an allowance for noise in a
    // quantity that already existed, not permission to introduce findings.
    const std::string path = writeBaseline(site("leakA", 1024, 100));
    const LeakReport report = reportWith({{"leakA", 1024, 100}, {"leakB", 8, 1}});

    auto diff = compareToBaseline(report, path, /*tolerancePercent=*/90.0);
    LH_CHECK(diff.value().regressed());
    LH_CHECK_EQ(diff.value().newSites.size(), std::size_t{1});
}

LH_TEST(Baseline, tolerance_does_not_excuse_a_new_mismatched_free) {
    // Undefined behaviour is not a quantity that drifts.
    const std::string path = writeBaseline(site("leakA", 1024, 100), /*mismatches=*/0);
    const LeakReport report = reportWith({{"leakA", 1024, 100}}, /*mismatches=*/1);

    auto diff = compareToBaseline(report, path, /*tolerancePercent=*/50.0);
    LH_CHECK(diff.value().regressed());
}

LH_TEST(Baseline, the_default_tolerance_is_exact) {
    const std::string path = writeBaseline(site("leakA", 1024, 100));
    const LeakReport report = reportWith({{"leakA", 1034, 101}});  // +1 block

    auto diff = compareToBaseline(report, path);  // no tolerance argument
    LH_CHECK(diff.value().regressed());
    LH_CHECK_EQ(diff.value().tolerancePercent, 0.0);
}
