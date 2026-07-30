/// CLI grammar, including the rule that the target's own flags are never
/// consumed by LeakHunter.

#include <sstream>
#include <string_view>
#include <vector>

#include "TestFramework.hpp"
#include "leakhunter/cli/CommandLineParser.hpp"

using leakhunter::cli::CommandLineParser;
using leakhunter::cli::Options;

namespace {

leakhunter::Result<Options> parse(std::vector<std::string_view> args) {
    static std::ostringstream out;
    static std::ostringstream err;
    out.str({});
    err.str({});

    const CommandLineParser parser(out, err);
    return parser.parse(args);
}

}  // namespace

LH_TEST(Cli, bare_program_generates_both_reports) {
    auto result = parse({"./app"});
    LH_CHECK(result.hasValue());

    const Options& options = result.value();
    LH_CHECK_EQ(options.targetCommand.size(), std::size_t{1});
    LH_CHECK_EQ(options.targetCommand[0], std::string{"./app"});
    LH_CHECK(options.emitHtml);
    LH_CHECK(options.emitJson);
}

LH_TEST(Cli, html_flag_selects_html_only) {
    auto result = parse({"--html", "./app"});
    LH_CHECK(result.hasValue());
    LH_CHECK(result.value().emitHtml);
    LH_CHECK(!result.value().emitJson);
}

LH_TEST(Cli, json_flag_selects_json_only) {
    auto result = parse({"--json", "./app"});
    LH_CHECK(result.hasValue());
    LH_CHECK(!result.value().emitHtml);
    LH_CHECK(result.value().emitJson);
}

LH_TEST(Cli, both_flags_together_select_both) {
    auto result = parse({"--html", "--json", "./app"});
    LH_CHECK(result.hasValue());
    LH_CHECK(result.value().emitHtml);
    LH_CHECK(result.value().emitJson);
}

LH_TEST(Cli, output_directory_is_honoured) {
    auto result = parse({"--output", "reports", "./app"});
    LH_CHECK(result.hasValue());
    LH_CHECK_EQ(result.value().outputDirectory.string(), std::string{"reports"});
}

LH_TEST(Cli, verbose_raises_the_log_level) {
    auto result = parse({"--verbose", "./app"});
    LH_CHECK(result.hasValue());
    LH_CHECK(result.value().verbosity == leakhunter::log::Level::Verbose);
}

LH_TEST(Cli, target_arguments_are_not_parsed_as_our_own) {
    // `--verbose` here belongs to ./app, not to leakhunter.
    auto result = parse({"./app", "--verbose", "--output", "x"});
    LH_CHECK(result.hasValue());

    const Options& options = result.value();
    LH_CHECK(options.verbosity == leakhunter::log::Level::Normal);
    LH_CHECK_EQ(options.targetCommand.size(), std::size_t{4});
    LH_CHECK_EQ(options.targetCommand[1], std::string{"--verbose"});
}

LH_TEST(Cli, double_dash_forces_the_split) {
    auto result = parse({"--verbose", "--", "./app", "--json"});
    LH_CHECK(result.hasValue());

    const Options& options = result.value();
    LH_CHECK(options.verbosity == leakhunter::log::Level::Verbose);
    LH_CHECK_EQ(options.targetCommand.size(), std::size_t{2});
    LH_CHECK_EQ(options.targetCommand[1], std::string{"--json"});
}

LH_TEST(Cli, missing_program_is_an_error) {
    auto result = parse({"--verbose"});
    LH_CHECK(!result.hasValue());
}

LH_TEST(Cli, unknown_option_is_an_error) {
    auto result = parse({"--nope", "./app"});
    LH_CHECK(!result.hasValue());
}

LH_TEST(Cli, option_missing_its_value_is_an_error) {
    auto result = parse({"--output"});
    LH_CHECK(!result.hasValue());
}

LH_TEST(Cli, max_frames_is_validated) {
    LH_CHECK(parse({"--max-frames", "64", "./app"}).hasValue());
    LH_CHECK(!parse({"--max-frames", "0", "./app"}).hasValue());
    LH_CHECK(!parse({"--max-frames", "9999", "./app"}).hasValue());
    LH_CHECK(!parse({"--max-frames", "32k", "./app"}).hasValue());
}

LH_TEST(Cli, help_requests_an_early_exit) {
    auto result = parse({"--help"});
    LH_CHECK(result.hasValue());
    LH_CHECK(result.value().shouldExitEarly);
    LH_CHECK_EQ(result.value().earlyExitCode, 0);
}

LH_TEST(Cli, runtime_blocks_are_excluded_unless_asked_for) {
    LH_CHECK(!parse({"./app"}).value().includeRuntimeLeaks);
    LH_CHECK(parse({"--include-runtime", "./app"}).value().includeRuntimeLeaks);
}

LH_TEST(Cli, source_resolution_is_on_by_default) {
    LH_CHECK(parse({"./app"}).value().resolveSourceLocations);
    LH_CHECK(!parse({"--no-source", "./app"}).value().resolveSourceLocations);
}

LH_TEST(Cli, min_leak_size_is_parsed_and_validated) {
    LH_CHECK_EQ(parse({"--min-leak-size", "4096", "./app"}).value().minLeakSize,
                std::uint64_t{4096});
    LH_CHECK(!parse({"--min-leak-size", "lots", "./app"}).hasValue());
}

LH_TEST(Cli, an_explicit_agent_path_is_recorded) {
    auto result = parse({"--agent", "/opt/lh/libleakhunter_agent.so", "./app"});
    LH_CHECK(result.hasValue());
    LH_CHECK_EQ(result.value().agentLibrary.string(),
                std::string{"/opt/lh/libleakhunter_agent.so"});
}

LH_TEST(Cli, mismatch_detection_is_on_by_default) {
    LH_CHECK(parse({"./app"}).value().detectMismatchedFrees);
    LH_CHECK(!parse({"--no-mismatch-check", "./app"}).value().detectMismatchedFrees);
}

LH_TEST(Cli, suppression_files_accumulate_in_order) {
    auto result = parse({"--suppressions", "a.supp", "--suppressions", "b.supp", "./app"});
    LH_CHECK(result.hasValue());

    const auto& files = result.value().suppressionFiles;
    LH_CHECK_EQ(files.size(), std::size_t{2});
    LH_CHECK_EQ(files[0].string(), std::string{"a.supp"});
    LH_CHECK_EQ(files[1].string(), std::string{"b.supp"});
}

LH_TEST(Cli, suppressions_needs_a_value) {
    LH_CHECK(!parse({"--suppressions"}).hasValue());
    LH_CHECK_EQ(parse({"./app"}).value().suppressionFiles.size(), std::size_t{0});
}

LH_TEST(Cli, strict_suppressions_is_opt_in) {
    LH_CHECK(!parse({"./app"}).value().strictSuppressions);
    LH_CHECK(parse({"--strict-suppressions", "./app"}).value().strictSuppressions);
}

LH_TEST(Cli, source_snippets_are_on_by_default) {
    LH_CHECK(parse({"./app"}).value().sourceSnippets);
    LH_CHECK(!parse({"--no-source-snippets", "./app"}).value().sourceSnippets);
}

LH_TEST(Cli, no_source_also_turns_off_snippets) {
    // Without file:line there is nothing to open, so the two flags can never be
    // left in a combination that promises source and cannot deliver it.
    const auto options = parse({"--no-source", "./app"}).value();
    LH_CHECK(!options.resolveSourceLocations);
    LH_CHECK(!options.sourceSnippets);
}

LH_TEST(Cli, snippet_context_is_parsed_and_bounded) {
    LH_CHECK_EQ(parse({"--snippet-context", "8", "./app"}).value().snippetContext,
                std::uint32_t{8});
    LH_CHECK_EQ(parse({"--snippet-context", "0", "./app"}).value().snippetContext,
                std::uint32_t{0});
    LH_CHECK(!parse({"--snippet-context", "33", "./app"}).hasValue());
    LH_CHECK(!parse({"--snippet-context", "lots", "./app"}).hasValue());
    LH_CHECK(!parse({"--snippet-context"}).hasValue());
}

LH_TEST(Cli, source_roots_accumulate_in_order) {
    const auto options =
        parse({"--source-root", "/a", "--source-root", "/b", "./app"}).value();
    LH_CHECK_EQ(options.sourceRoots.size(), std::size_t{2});
    LH_CHECK_EQ(options.sourceRoots[0].string(), std::string{"/a"});
    LH_CHECK_EQ(options.sourceRoots[1].string(), std::string{"/b"});
    LH_CHECK(!parse({"--source-root"}).hasValue());
}

LH_TEST(Cli, diagnostics_is_opt_in) {
    LH_CHECK(!parse({"./app"}).value().emitDiagnostics);
    LH_CHECK(parse({"--diagnostics", "./app"}).value().emitDiagnostics);
}

LH_TEST(Cli, trace_file_implies_keeping_it) {
    auto result = parse({"--trace-file", "/tmp/x.lhtrace", "./app"});
    LH_CHECK(result.hasValue());
    LH_CHECK(result.value().keepTrace);
}
