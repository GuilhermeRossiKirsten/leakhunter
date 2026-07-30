/// Report rendering: schema stability for JSON, self-containment and escaping
/// for HTML.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "TestFramework.hpp"
#include "leakhunter/report/HtmlReportGenerator.hpp"
#include "leakhunter/report/JsonReportGenerator.hpp"

namespace fs = std::filesystem;
using leakhunter::analysis::Leak;
using leakhunter::analysis::LeakGroup;
using leakhunter::analysis::LeakReport;
using leakhunter::report::HtmlReportGenerator;
using leakhunter::report::JsonReportGenerator;

namespace {

LeakReport makeReport() {
    LeakReport report;
    report.toolVersion = "0.1.0";
    report.generatedAtIso8601 = "2024-01-01T00:00:00Z";
    report.targetCommand = "./app --flag";
    report.stats.pid = 1234;
    report.stats.totalAllocations = 500;
    report.stats.totalDeallocations = 498;
    report.stats.totalBytesAllocated = 100000;
    report.stats.totalBytesFreed = 90000;
    report.stats.peakLiveBytes = 20000;
    report.leakedBytes = 10000;
    report.leakCount = 2;

    leakhunter::StackFrame frame;
    frame.address = 0x401234;
    frame.moduleBase = 0x400000;
    frame.function = "allocateBuffer";
    frame.module = "/home/dev/app";
    frame.file = "/home/dev/app/src/buffer.cpp";
    frame.line = 42;
    frame.resolved = true;

    Leak leak;
    leak.address = 0xAAAA;
    leak.size = 8000;
    leak.threadId = 7;
    leak.kind = leakhunter::AllocationKind::Malloc;
    leak.trace = {frame};
    leak.responsibleFrame = 0;
    report.leaks.push_back(leak);

    leak.address = 0xBBBB;
    leak.size = 2000;
    report.leaks.push_back(leak);

    LeakGroup group;
    group.function = "allocateBuffer";
    group.module = "/home/dev/app";
    group.location = "/home/dev/app/src/buffer.cpp:42";
    group.totalBytes = 10000;
    group.count = 2;
    group.threadCount = 1;
    group.representativeTrace = {frame};
    group.leakIndices = {0, 1};
    report.groups.push_back(group);

    return report;
}

struct TempDir {
    fs::path path;

    TempDir() : path(fs::temp_directory_path() / "leakhunter-report-test") {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace

LH_TEST(JsonReport, contains_the_documented_schema) {
    const nlohmann::json document = JsonReportGenerator::toJson(makeReport());

    LH_CHECK_EQ(document["schemaVersion"].get<int>(), 2);
    LH_CHECK_EQ(document["tool"]["name"].get<std::string>(), std::string{"leakhunter"});
    LH_CHECK_EQ(document["summary"]["leakCount"].get<std::uint64_t>(), std::uint64_t{2});
    LH_CHECK_EQ(document["summary"]["leakedBytes"].get<std::uint64_t>(), std::uint64_t{10000});
    LH_CHECK_EQ(document["summary"]["clean"].get<bool>(), false);
    LH_CHECK_EQ(document["groups"].size(), std::size_t{1});
    LH_CHECK_EQ(document["leaks"].size(), std::size_t{2});
    LH_CHECK_EQ(document["groups"][0]["function"].get<std::string>(),
                std::string{"allocateBuffer"});
    LH_CHECK_EQ(document["leaks"][0]["stackTrace"][0]["line"].get<int>(), 42);
}

LH_TEST(JsonReport, is_written_and_parses_back) {
    const TempDir temp;
    JsonReportGenerator generator;

    const fs::path path = temp.path / "report.json";
    LH_CHECK(generator.generate(makeReport(), path).hasValue());
    LH_CHECK(fs::exists(path));

    const auto parsed = nlohmann::json::parse(readFile(path));
    LH_CHECK_EQ(parsed["summary"]["leakCount"].get<std::uint64_t>(), std::uint64_t{2});
}

LH_TEST(JsonReport, a_clean_run_is_marked_clean) {
    LeakReport report;
    report.toolVersion = "0.1.0";

    const nlohmann::json document = JsonReportGenerator::toJson(report);
    LH_CHECK_EQ(document["summary"]["clean"].get<bool>(), true);
    LH_CHECK_EQ(document["leaks"].size(), std::size_t{0});
}

LH_TEST(HtmlReport, is_a_self_contained_document) {
    const std::string html = HtmlReportGenerator::render(makeReport());

    LH_CHECK(html.find("<!DOCTYPE html>") != std::string::npos);
    LH_CHECK(html.find("allocateBuffer") != std::string::npos);
    LH_CHECK(html.find("leakhunter-data") != std::string::npos);

    // No external resources: the file has to work from a CI artifact or an
    // email attachment, offline.
    LH_CHECK(html.find("http://") == std::string::npos);
    LH_CHECK(html.find("https://") == std::string::npos);
    LH_CHECK(html.find("<script src") == std::string::npos);
    LH_CHECK(html.find("<link rel=\"stylesheet\"") == std::string::npos);
}

LH_TEST(HtmlReport, escapes_the_command_line) {
    LeakReport report = makeReport();
    report.targetCommand = "./app <script>alert(1)</script>";

    const std::string html = HtmlReportGenerator::render(report);

    LH_CHECK(html.find("<script>alert(1)</script>") == std::string::npos);
    LH_CHECK(html.find("&lt;script&gt;") != std::string::npos);
}

LH_TEST(HtmlReport, embedded_json_cannot_close_the_script_tag) {
    LeakReport report = makeReport();
    report.groups[0].function = "evil</script><script>alert(1)</script>";

    const std::string html = HtmlReportGenerator::render(report);
    const std::size_t dataStart = html.find("id=\"leakhunter-data\"");
    LH_CHECK(dataStart != std::string::npos);

    // Between the data tag and its closing tag there must be no raw "</script".
    const std::size_t closing = html.find("</script>", dataStart);
    const std::string payload = html.substr(dataStart, closing - dataStart);
    LH_CHECK(payload.find("</script") == std::string::npos);
}

LH_TEST(HtmlReport, a_clean_run_says_so) {
    LeakReport report;
    report.toolVersion = "0.1.0";
    report.targetCommand = "./clean";

    const std::string html = HtmlReportGenerator::render(report);
    // The verdict has to be an answer, not a statistic: someone opening this
    // wants to know whether the run passed.
    LH_CHECK(html.find("PASSED") != std::string::npos);
    LH_CHECK(html.find("verdict good") != std::string::npos);
}

LH_TEST(JsonReport, malformed_utf8_does_not_destroy_the_report) {
    // Symbol names come from arbitrary binaries and module paths are just bytes
    // on Linux. nlohmann throws on the first invalid sequence by default, which
    // would turn one odd symbol into "no report at all".
    LeakReport report = makeReport();
    report.groups[0].function = "bad\xC3\x28name";
    report.groups[0].module = "/opt/\xFF\xFElib.so";
    // Split literal: "\xA1broken" would swallow the 'b' as a hex digit and
    // overflow the escape. This is exactly the footgun -Werror catches.
    report.leaks[0].trace[0].function = "also\xE2\x28\xA1" "broken";

    const std::string serialized =
        JsonReportGenerator::serialize(JsonReportGenerator::toJson(report), true);

    LH_CHECK(!serialized.empty());

    // It must still be parseable, which is the whole point.
    const auto parsed = nlohmann::json::parse(serialized);
    LH_CHECK_EQ(parsed["summary"]["leakCount"].get<std::uint64_t>(), std::uint64_t{2});
    LH_CHECK(parsed["groups"][0]["function"].get<std::string>().find("name") !=
             std::string::npos);
}

LH_TEST(HtmlReport, malformed_utf8_still_renders) {
    LeakReport report = makeReport();
    report.groups[0].function = "bad\xC3\x28name";

    const std::string html = HtmlReportGenerator::render(report);
    LH_CHECK(html.find("<!DOCTYPE html>") != std::string::npos);
    LH_CHECK(html.find("leakhunter-data") != std::string::npos);
}

LH_TEST(HtmlReport, every_sort_key_in_the_markup_exists_in_the_data) {
    // A header whose data-key names no field sorts by undefined, silently
    // scrambling the table. Checking the contract here is the closest thing to
    // running the script without a JS engine in the test environment.
    const std::string html = HtmlReportGenerator::render(makeReport());
    const nlohmann::json document = JsonReportGenerator::toJson(makeReport());

    std::size_t position = 0;
    std::size_t checked = 0;
    while ((position = html.find("data-key=\"", position)) != std::string::npos) {
        position += 10;
        const std::size_t end = html.find('"', position);
        LH_CHECK(end != std::string::npos);

        const std::string key = html.substr(position, end - position);
        ++checked;

        // frameCount is derived by the script from stackTrace; everything else
        // has to be a real field of a group object.
        if (key != "frameCount") {
            LH_CHECK(document["groups"][0].contains(key));
        }
    }
    LH_CHECK(checked >= 5);
}

// --- mismatched frees ------------------------------------------------------

namespace {

/// A report with no leaks at all, only a `new[]` released with `free()`.
LeakReport makeMismatchReport() {
    LeakReport report;
    report.toolVersion = "0.1.0";
    report.generatedAtIso8601 = "2024-01-01T00:00:00Z";
    report.targetCommand = "./app";
    report.stats.totalAllocations = 10;
    report.stats.totalDeallocations = 10;
    report.stats.mismatchedFrees = 1;

    leakhunter::StackFrame frame;
    frame.address = 0x401234;
    frame.function = "buildTable";
    frame.module = "/home/dev/app";
    frame.file = "/home/dev/app/src/table.cpp";
    frame.line = 88;
    frame.resolved = true;

    leakhunter::MismatchedFree mismatch;
    mismatch.address = 0xCCCC;
    mismatch.size = 400;
    mismatch.allocatedBy = leakhunter::AllocationKind::NewArray;
    mismatch.releasedBy = leakhunter::ReleaseKind::Free;
    mismatch.allocatedOnThread = 1;
    mismatch.releasedOnThread = 2;
    mismatch.trace = {frame};
    report.mismatchedFrees.push_back(mismatch);

    return report;
}

}  // namespace

LH_TEST(JsonReport, a_mismatched_free_is_described_in_full) {
    const nlohmann::json document = JsonReportGenerator::toJson(makeMismatchReport());

    LH_CHECK_EQ(document["mismatchedFrees"].size(), std::size_t{1});
    LH_CHECK_EQ(document["summary"]["mismatchedFreeCount"].get<std::uint64_t>(),
                std::uint64_t{1});

    const auto& mismatch = document["mismatchedFrees"][0];
    LH_CHECK_EQ(mismatch["size"].get<std::uint64_t>(), std::uint64_t{400});
    LH_CHECK_EQ(mismatch["allocatedBy"].get<std::string>(), std::string{"operator new[]"});
    LH_CHECK_EQ(mismatch["releasedBy"].get<std::string>(), std::string{"free"});
    LH_CHECK_EQ(mismatch["description"].get<std::string>(),
                std::string{"allocated with new[], released with free()"});
    LH_CHECK_EQ(mismatch["responsibleFunction"].get<std::string>(), std::string{"buildTable"});
    LH_CHECK_EQ(mismatch["stackTrace"].size(), std::size_t{1});
}

LH_TEST(JsonReport, a_run_that_only_mismatches_is_not_clean) {
    const nlohmann::json document = JsonReportGenerator::toJson(makeMismatchReport());

    LH_CHECK_EQ(document["summary"]["leakCount"].get<std::uint64_t>(), std::uint64_t{0});
    LH_CHECK_EQ(document["summary"]["clean"].get<bool>(), false);
}

LH_TEST(JsonReport, a_clean_run_still_carries_the_empty_array) {
    // Consumers should be able to read report.mismatchedFrees unconditionally.
    LeakReport report;
    report.toolVersion = "0.1.0";

    const nlohmann::json document = JsonReportGenerator::toJson(report);
    LH_CHECK(document.contains("mismatchedFrees"));
    LH_CHECK(document["mismatchedFrees"].is_array());
    LH_CHECK_EQ(document["mismatchedFrees"].size(), std::size_t{0});
}

LH_TEST(HtmlReport, a_mismatch_gets_its_own_section) {
    const std::string html = HtmlReportGenerator::render(makeMismatchReport());

    LH_CHECK(html.find("Mismatched frees") != std::string::npos);
    LH_CHECK(html.find("id=\"mismatch-section\"") != std::string::npos);
    LH_CHECK(html.find("id=\"mismatch-body\"") != std::string::npos);

    // The verdict must not claim the run is fine just because nothing leaked.
    LH_CHECK(html.find("verdict good") == std::string::npos);
    LH_CHECK(html.find("undefined behaviour") != std::string::npos);
}

LH_TEST(HtmlReport, a_clean_run_keeps_the_mismatch_section_hidden) {
    LeakReport report;
    report.toolVersion = "0.1.0";

    const std::string html = HtmlReportGenerator::render(report);
    LH_CHECK(html.find("verdict good") != std::string::npos);
    LH_CHECK(html.find(R"(<section id="mismatch-section" class="hidden">)") !=
             std::string::npos);
}

LH_TEST(JsonReport, the_state_of_the_check_is_recorded_not_implied) {
    // Zero findings means "clean" only when the check actually ran.
    LeakReport report;
    report.toolVersion = "0.1.0";

    report.mismatchCheck = leakhunter::analysis::MismatchCheck::Active;
    LH_CHECK_EQ(JsonReportGenerator::toJson(report)["summary"]["mismatchDetection"]
                    .get<std::string>(),
                std::string{"active"});

    report.mismatchCheck = leakhunter::analysis::MismatchCheck::Suppressed;
    LH_CHECK_EQ(JsonReportGenerator::toJson(report)["summary"]["mismatchDetection"]
                    .get<std::string>(),
                std::string{"suppressed"});

    report.mismatchCheck = leakhunter::analysis::MismatchCheck::Disabled;
    LH_CHECK_EQ(JsonReportGenerator::toJson(report)["summary"]["mismatchDetection"]
                    .get<std::string>(),
                std::string{"disabled"});
}

LH_TEST(HtmlReport, a_suppressed_check_says_so_instead_of_claiming_clean) {
    LeakReport report;
    report.toolVersion = "0.1.0";
    report.mismatchCheck = leakhunter::analysis::MismatchCheck::Suppressed;

    const std::string html = HtmlReportGenerator::render(report);
    LH_CHECK(html.find("Mismatched frees were not checked") != std::string::npos);
}

LH_TEST(HtmlReport, an_active_check_adds_no_notice) {
    LeakReport report;
    report.toolVersion = "0.1.0";

    const std::string html = HtmlReportGenerator::render(report);
    LH_CHECK(html.find("were not checked") == std::string::npos);
}

LH_TEST(HtmlReport, leaks_and_mismatches_are_both_announced) {
    LeakReport report = makeReport();
    report.stats.mismatchedFrees = 3;

    const std::string html = HtmlReportGenerator::render(report);
    LH_CHECK(html.find("2 leaks") != std::string::npos);
    LH_CHECK(html.find("3 mismatched frees") != std::string::npos);
}

// --- source snippets -------------------------------------------------------

namespace {

leakhunter::SourceSnippet makeSnippet() {
    leakhunter::SourceSnippet snippet;
    snippet.file = "/home/dev/app/src/buffer.cpp";
    snippet.firstLine = 40;
    snippet.blamedLine = 42;
    snippet.column = 20;
    snippet.lines = {"void allocateBuffer() {", "    // grab some room",
                     "    char* p = (char*)malloc(1024);", "    use(p);", "}"};
    return snippet;
}

}  // namespace

LH_TEST(JsonReport, a_snippet_is_serialised_with_its_line_numbers) {
    LeakReport report = makeReport();
    report.groups[0].snippet = makeSnippet();

    const nlohmann::json document = JsonReportGenerator::toJson(report);
    const auto& snippet = document["groups"][0]["snippet"];

    LH_CHECK_EQ(snippet["firstLine"].get<std::uint32_t>(), std::uint32_t{40});
    LH_CHECK_EQ(snippet["blamedLine"].get<std::uint32_t>(), std::uint32_t{42});
    LH_CHECK_EQ(snippet["column"].get<std::uint32_t>(), std::uint32_t{20});
    LH_CHECK_EQ(snippet["lines"].size(), std::size_t{5});
    LH_CHECK_EQ(snippet["file"].get<std::string>(),
                std::string{"/home/dev/app/src/buffer.cpp"});
}

LH_TEST(JsonReport, no_snippet_means_the_key_is_absent_not_null) {
    // A consumer testing `"snippet" in group` should get a straight answer.
    const nlohmann::json document = JsonReportGenerator::toJson(makeReport());
    LH_CHECK(!document["groups"][0].contains("snippet"));
}

LH_TEST(JsonReport, a_frame_column_appears_only_when_known) {
    LeakReport report = makeReport();
    report.groups[0].representativeTrace[0].column = 17;

    const nlohmann::json withColumn = JsonReportGenerator::toJson(report);
    LH_CHECK_EQ(withColumn["groups"][0]["stackTrace"][0]["column"].get<std::uint32_t>(),
                std::uint32_t{17});

    // addr2line reports none; a fabricated 0 would be worse than an absent key.
    const nlohmann::json without = JsonReportGenerator::toJson(makeReport());
    LH_CHECK(!without["groups"][0]["stackTrace"][0].contains("column"));
}

LH_TEST(HtmlReport, a_snippet_reaches_the_page_with_its_blamed_line) {
    LeakReport report = makeReport();
    report.groups[0].snippet = makeSnippet();

    const std::string html = HtmlReportGenerator::render(report);

    // The data has to be embedded and the CSS that highlights it has to exist,
    // or the page renders source with nothing marked.
    LH_CHECK(html.find("\"blamedLine\":42") != std::string::npos);
    LH_CHECK(html.find("malloc(1024)") != std::string::npos);
    LH_CHECK(html.find(".snippet tr.blamed") != std::string::npos);
    LH_CHECK(html.find("snippetHtml") != std::string::npos);
}

LH_TEST(HtmlReport, source_code_is_escaped_before_it_reaches_the_markup) {
    // The single most important test of this feature. Source is arbitrary text
    // read off disk and placed into a page; if it is not escaped, a comment in
    // the analysed program becomes script in the report.
    LeakReport report = makeReport();
    leakhunter::SourceSnippet snippet = makeSnippet();
    snippet.lines = {"template <class T> struct S;  // a < b && c > d",
                     "auto* p = new T<int>();  /* </script><script>alert(1)</script> */",
                     "const char* q = \"quoted\";"};
    snippet.firstLine = 1;
    snippet.blamedLine = 2;
    report.groups[0].snippet = snippet;

    const std::string html = HtmlReportGenerator::render(report);

    // Nothing may close the data element early, and no raw script tag may exist.
    const std::size_t dataStart = html.find("id=\"leakhunter-data\"");
    const std::size_t closing = html.find("</script>", dataStart);
    LH_CHECK(html.substr(dataStart, closing - dataStart).find("</script") == std::string::npos);
    LH_CHECK(html.find("<script>alert(1)</script>") == std::string::npos);
}

LH_TEST(HtmlReport, a_mismatched_free_carries_its_snippet_too) {
    LeakReport report = makeMismatchReport();
    report.mismatchedFrees[0].snippet = makeSnippet();

    const std::string html = HtmlReportGenerator::render(report);
    LH_CHECK(html.find("malloc(1024)") != std::string::npos);
    LH_CHECK(html.find("allocated here") != std::string::npos);
}

LH_TEST(HtmlReport, is_written_to_disk) {
    const TempDir temp;
    HtmlReportGenerator generator;

    const fs::path path = temp.path / "report.html";
    LH_CHECK(generator.generate(makeReport(), path).hasValue());
    LH_CHECK(fs::exists(path));
    LH_CHECK(fs::file_size(path) > 1000);
}
