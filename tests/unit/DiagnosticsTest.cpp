/// Compiler-style diagnostics.
///
/// These lines are parsed by editors and by GitHub Actions, so the format is a
/// contract with software rather than a message to a person.

#include <sstream>
#include <string>

#include "TestFramework.hpp"
#include "leakhunter/report/DiagnosticsWriter.hpp"

using leakhunter::MismatchedFree;
using leakhunter::analysis::LeakGroup;
using leakhunter::analysis::LeakReport;
using leakhunter::report::DiagnosticStyle;
using leakhunter::report::writeDiagnostics;

namespace {

leakhunter::StackFrame frame(std::string file, std::uint32_t line, std::uint32_t column,
                             std::string function = "doWork") {
    leakhunter::StackFrame result;
    result.function = std::move(function);
    result.module = "/home/dev/app";
    result.file = std::move(file);
    result.line = line;
    result.column = column;
    result.resolved = true;
    return result;
}

LeakGroup group(const leakhunter::StackFrame& blamed, std::uint64_t count, std::uint64_t bytes) {
    LeakGroup g;
    g.function = blamed.function;
    g.count = count;
    g.totalBytes = bytes;
    g.representativeTrace = {blamed};
    g.blamedFrame = 0;
    return g;
}

MismatchedFree mismatch(const leakhunter::StackFrame& blamed, std::uint64_t size) {
    MismatchedFree m;
    m.size = size;
    m.allocatedBy = leakhunter::AllocationKind::NewArray;
    m.releasedBy = leakhunter::ReleaseKind::Free;
    m.trace = {blamed};
    m.responsibleFrame = 0;
    return m;
}

std::string render(const LeakReport& report, DiagnosticStyle style) {
    std::ostringstream out;
    writeDiagnostics(report, out, style);
    return out.str();
}

}  // namespace

LH_TEST(Diagnostics, gcc_style_is_what_an_editor_can_parse) {
    LeakReport report;
    report.groups.push_back(group(frame("/home/dev/app/src/a.cpp", 27, 60), 100, 204800));

    const std::string text = render(report, DiagnosticStyle::Gcc);

    // file:line:column: warning: -- the shape every editor already understands.
    LH_CHECK(text.rfind("/home/dev/app/src/a.cpp:27:60: warning: leak:", 0) == 0);
    LH_CHECK(text.find("100 block(s)") != std::string::npos);
    LH_CHECK(text.find("200.00 KiB") != std::string::npos);
    LH_CHECK(text.find("[leakhunter:leak]") != std::string::npos);
}

LH_TEST(Diagnostics, a_missing_column_is_omitted_not_faked) {
    // addr2line gives no column. Emitting ":0" would send an editor to column
    // zero, which some of them treat as an error.
    LeakReport report;
    report.groups.push_back(group(frame("/home/dev/app/src/a.cpp", 27, 0), 1, 64));

    const std::string text = render(report, DiagnosticStyle::Gcc);
    LH_CHECK(text.rfind("/home/dev/app/src/a.cpp:27: warning:", 0) == 0);
    LH_CHECK(text.find(":27:0:") == std::string::npos);
}

LH_TEST(Diagnostics, github_style_is_a_workflow_command) {
    LeakReport report;
    report.groups.push_back(group(frame("src/a.cpp", 27, 60), 100, 204800));

    const std::string text = render(report, DiagnosticStyle::GitHubActions);
    LH_CHECK(text.rfind("::warning file=src/a.cpp,line=27,col=60", 0) == 0);
    LH_CHECK(text.find("::100 block(s)") != std::string::npos);
}

LH_TEST(Diagnostics, github_style_cannot_be_broken_by_a_cpp_name) {
    // "::" terminates a workflow command's parameter list. Every demangled C++
    // name is full of them, so an unescaped one would truncate the annotation.
    LeakReport report;
    report.groups.push_back(
        group(frame("src/a.cpp", 5, 1, "poc::detail::Cache::warmUp(int)"), 1, 64));

    const std::string text = render(report, DiagnosticStyle::GitHubActions);

    const std::size_t separator = text.find("::", text.find("col=1"));
    LH_CHECK(separator != std::string::npos);
    // After the one real separator there must be no further "::" to confuse the
    // parser.
    LH_CHECK(text.find("::", separator + 2) == std::string::npos);
    // ...and the name is still readable.
    LH_CHECK(text.find("Cache") != std::string::npos);
    LH_CHECK(text.find("warmUp") != std::string::npos);
}

LH_TEST(Diagnostics, mismatched_frees_at_one_site_collapse_to_one_line) {
    // A site that gets the pairing wrong gets it wrong repeatedly. Eight
    // identical annotations on one line of a pull request is noise.
    LeakReport report;
    const auto blamed = frame("/home/dev/app/src/cache.cpp", 67, 49, "copyPayload");
    for (int i = 0; i < 8; ++i) {
        report.mismatchedFrees.push_back(mismatch(blamed, 512));
    }

    const std::string text = render(report, DiagnosticStyle::Gcc);

    std::size_t lines = 0;
    for (const char character : text) {
        lines += character == '\n' ? 1 : 0;
    }
    LH_CHECK_EQ(lines, std::size_t{1});
    LH_CHECK(text.find("8 block(s)") != std::string::npos);
    LH_CHECK(text.find("4.00 KiB affected") != std::string::npos);
    LH_CHECK(text.find("new[]") != std::string::npos);
    LH_CHECK(text.find("free()") != std::string::npos);
}

LH_TEST(Diagnostics, mismatches_at_different_sites_stay_separate) {
    LeakReport report;
    report.mismatchedFrees.push_back(mismatch(frame("a.cpp", 10, 1), 16));
    report.mismatchedFrees.push_back(mismatch(frame("a.cpp", 20, 1), 16));
    report.mismatchedFrees.push_back(mismatch(frame("b.cpp", 10, 1), 16));

    const std::string text = render(report, DiagnosticStyle::Gcc);
    LH_CHECK(text.find("a.cpp:10:1") != std::string::npos);
    LH_CHECK(text.find("a.cpp:20:1") != std::string::npos);
    LH_CHECK(text.find("b.cpp:10:1") != std::string::npos);
}

LH_TEST(Diagnostics, findings_without_a_location_are_counted_aloud) {
    // Silently dropping them would make this stream disagree with the report
    // printed next to it.
    LeakReport report;
    report.groups.push_back(group(frame("", 0, 0, "strippedFunction"), 5, 500));
    report.groups.push_back(group(frame("known.cpp", 3, 1), 1, 64));

    const std::string text = render(report, DiagnosticStyle::Gcc);
    LH_CHECK(text.find("known.cpp:3:1") != std::string::npos);
    LH_CHECK(text.find("1 finding(s) had no source location") != std::string::npos);
    // And never a bogus line 0 for the one we cannot place.
    LH_CHECK(text.find(":0:") == std::string::npos);
}

LH_TEST(Diagnostics, a_clean_report_writes_nothing_at_all) {
    const LeakReport report;
    LH_CHECK(render(report, DiagnosticStyle::Gcc).empty());
    LH_CHECK(render(report, DiagnosticStyle::GitHubActions).empty());
}

LH_TEST(Diagnostics, a_group_whose_blamed_index_is_out_of_range_is_skipped) {
    LeakReport report;
    LeakGroup broken;
    broken.function = "mystery";
    broken.count = 1;
    broken.blamedFrame = 7;  // no such frame
    report.groups.push_back(broken);

    const std::string text = render(report, DiagnosticStyle::Gcc);
    LH_CHECK(text.find("1 finding(s) had no source location") != std::string::npos);
}
