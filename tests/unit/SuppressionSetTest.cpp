/// Suppression parsing and matching.

#include <string>

#include "TestFramework.hpp"
#include "leakhunter/analysis/SuppressionSet.hpp"

using leakhunter::StackFrame;
using leakhunter::StackTrace;
using leakhunter::analysis::SuppressionScope;
using leakhunter::analysis::SuppressionSet;

namespace {

StackFrame frame(std::string function, std::string module, std::string file = {}) {
    StackFrame result;
    result.function = std::move(function);
    result.module = std::move(module);
    result.file = std::move(file);
    result.line = result.file.empty() ? 0 : 42;
    result.resolved = true;
    return result;
}

/// blamed frame = index 0, then a caller in the application.
StackTrace vendorStack() {
    return {
        frame("vendorAlloc", "/opt/vendor/libparser.so", "/opt/vendor/src/parser.cpp"),
        frame("myLoadConfig", "/home/dev/app", "/home/dev/app/src/config.cpp"),
        frame("main", "/home/dev/app", "/home/dev/app/src/main.cpp"),
    };
}

SuppressionSet parse(const std::string& text) {
    SuppressionSet set;
    const auto status = set.loadText(text, "test.supp");
    LH_CHECK(status.hasValue());
    return set;
}

}  // namespace

// --- parsing ---------------------------------------------------------------

LH_TEST(Suppressions, comments_and_blank_lines_are_ignored) {
    const SuppressionSet set = parse(
        "# a comment\n"
        "\n"
        "   \n"
        "function:foo\n"
        "   # indented comment\n"
        "\tmodule:bar\t\n");

    LH_CHECK_EQ(set.size(), std::size_t{2});
    LH_CHECK(set.rules()[0].scope == SuppressionScope::Function);
    LH_CHECK_EQ(set.rules()[0].pattern, std::string{"foo"});
    LH_CHECK(set.rules()[1].scope == SuppressionScope::Module);
    LH_CHECK_EQ(set.rules()[1].pattern, std::string{"bar"});
}

LH_TEST(Suppressions, every_scope_keyword_parses) {
    const SuppressionSet set = parse("function:a\nmodule:b\nfile:c\nstack:d\n");
    LH_CHECK_EQ(set.size(), std::size_t{4});
    LH_CHECK(set.rules()[0].scope == SuppressionScope::Function);
    LH_CHECK(set.rules()[1].scope == SuppressionScope::Module);
    LH_CHECK(set.rules()[2].scope == SuppressionScope::File);
    LH_CHECK(set.rules()[3].scope == SuppressionScope::Stack);
}

LH_TEST(Suppressions, a_pattern_may_contain_colons) {
    // C++ symbols are full of them; only the first colon separates.
    const SuppressionSet set = parse("function:mylib::detail::*\n");
    LH_CHECK_EQ(set.rules()[0].pattern, std::string{"mylib::detail::*"});
}

LH_TEST(Suppressions, line_numbers_are_recorded_for_diagnostics) {
    const SuppressionSet set = parse("# one\n# two\nfunction:foo\n");
    LH_CHECK_EQ(set.rules()[0].line, std::uint32_t{3});
    LH_CHECK_EQ(set.rules()[0].describe(), std::string{"test.supp:3: function:foo"});
}

LH_TEST(Suppressions, a_missing_colon_is_an_error_not_a_warning) {
    SuppressionSet set;
    const auto status = set.loadText("function foo\n", "test.supp");
    LH_CHECK(!status.hasValue());
    // The message has to name the line, or the user cannot find it.
    LH_CHECK(status.message().find("test.supp:1") != std::string::npos);
}

LH_TEST(Suppressions, an_unknown_scope_is_an_error) {
    SuppressionSet set;
    const auto status = set.loadText("functoin:foo\n", "test.supp");
    LH_CHECK(!status.hasValue());
    LH_CHECK(status.message().find("functoin") != std::string::npos);
    // And it must list what *is* valid.
    LH_CHECK(status.message().find("stack") != std::string::npos);
}

LH_TEST(Suppressions, an_empty_pattern_is_an_error) {
    SuppressionSet set;
    LH_CHECK(!set.loadText("function:\n", "test.supp").hasValue());
    LH_CHECK(!set.loadText("stack:   \n", "test.supp").hasValue());
}

LH_TEST(Suppressions, a_file_without_a_trailing_newline_still_parses) {
    const SuppressionSet set = parse("function:foo");
    LH_CHECK_EQ(set.size(), std::size_t{1});
}

// --- matching --------------------------------------------------------------

LH_TEST(Suppressions, a_function_rule_matches_only_the_blamed_frame) {
    const SuppressionSet set = parse("function:vendorAlloc\n");
    LH_CHECK_EQ(set.match(vendorStack(), 0), std::size_t{0});

    // Blame the application frame instead: the vendor frame is still in the
    // stack, but the rule must not reach it.
    LH_CHECK_EQ(set.match(vendorStack(), 1), SuppressionSet::npos);
}

LH_TEST(Suppressions, a_module_rule_matches_the_blamed_frames_object) {
    const SuppressionSet set = parse("module:*/libparser.so\n");
    LH_CHECK_EQ(set.match(vendorStack(), 0), std::size_t{0});
    LH_CHECK_EQ(set.match(vendorStack(), 1), SuppressionSet::npos);
}

LH_TEST(Suppressions, a_file_rule_matches_the_blamed_frames_source) {
    const SuppressionSet set = parse("file:/opt/vendor/*\n");
    LH_CHECK_EQ(set.match(vendorStack(), 0), std::size_t{0});
    LH_CHECK_EQ(set.match(vendorStack(), 1), SuppressionSet::npos);
}

LH_TEST(Suppressions, a_stack_rule_reaches_any_frame) {
    // This is the one that solves "anything allocated under this library".
    const SuppressionSet set = parse("stack:*/libparser.so\n");
    LH_CHECK_EQ(set.match(vendorStack(), 0), std::size_t{0});
    LH_CHECK_EQ(set.match(vendorStack(), 1), std::size_t{0});
    LH_CHECK_EQ(set.match(vendorStack(), 2), std::size_t{0});
}

LH_TEST(Suppressions, a_stack_rule_matches_function_module_or_file) {
    LH_CHECK_EQ(parse("stack:vendorAlloc\n").match(vendorStack(), 2), std::size_t{0});
    LH_CHECK_EQ(parse("stack:*/libparser.so\n").match(vendorStack(), 2), std::size_t{0});
    LH_CHECK_EQ(parse("stack:/opt/vendor/*\n").match(vendorStack(), 2), std::size_t{0});
    LH_CHECK_EQ(parse("stack:nothing-like-this\n").match(vendorStack(), 2),
                SuppressionSet::npos);
}

LH_TEST(Suppressions, the_first_matching_rule_wins) {
    const SuppressionSet set = parse("function:nope\nfunction:vendorAlloc\nstack:*\n");
    LH_CHECK_EQ(set.match(vendorStack(), 0), std::size_t{1});
}

LH_TEST(Suppressions, an_out_of_range_blamed_frame_is_not_a_crash) {
    const SuppressionSet set = parse("function:vendorAlloc\n");
    LH_CHECK_EQ(set.match(vendorStack(), 99), SuppressionSet::npos);
    LH_CHECK_EQ(set.match({}, 0), SuppressionSet::npos);
}

LH_TEST(Suppressions, an_unresolved_frame_does_not_match_by_accident) {
    // Empty function/module/file must not be matched by `*`-only patterns in a
    // way that silently swallows everything unresolvable... except that `*`
    // does match the empty string, which is correct and worth pinning down so
    // the behaviour is a decision rather than an accident.
    const StackTrace unresolved = {StackFrame{}};
    LH_CHECK_EQ(parse("function:*\n").match(unresolved, 0), std::size_t{0});
    LH_CHECK_EQ(parse("function:something\n").match(unresolved, 0), SuppressionSet::npos);
}

// --- hit accounting --------------------------------------------------------

LH_TEST(Suppressions, hits_are_counted_per_rule) {
    SuppressionSet set = parse("function:vendorAlloc\nfunction:neverFires\n");

    set.noteHit(0);
    set.noteHit(0);
    set.noteHit(0);

    LH_CHECK_EQ(set.rules()[0].hits, std::uint64_t{3});
    LH_CHECK_EQ(set.rules()[1].hits, std::uint64_t{0});
}

LH_TEST(Suppressions, unused_rules_are_reported) {
    SuppressionSet set = parse("function:a\nfunction:b\nfunction:c\n");
    set.noteHit(1);

    const auto unused = set.unusedRules();
    LH_CHECK_EQ(unused.size(), std::size_t{2});
    LH_CHECK_EQ(unused[0]->pattern, std::string{"a"});
    LH_CHECK_EQ(unused[1]->pattern, std::string{"c"});
}

LH_TEST(Suppressions, noting_a_hit_out_of_range_is_ignored) {
    SuppressionSet set = parse("function:a\n");
    set.noteHit(99);  // must not corrupt anything
    LH_CHECK_EQ(set.rules()[0].hits, std::uint64_t{0});
}

LH_TEST(Suppressions, loading_two_files_appends_and_keeps_provenance) {
    SuppressionSet set;
    LH_CHECK(set.loadText("function:a\n", "first.supp").hasValue());
    LH_CHECK(set.loadText("function:b\n", "second.supp").hasValue());

    LH_CHECK_EQ(set.size(), std::size_t{2});
    LH_CHECK_EQ(set.rules()[0].origin, std::string{"first.supp"});
    LH_CHECK_EQ(set.rules()[1].origin, std::string{"second.supp"});
    LH_CHECK_EQ(set.rules()[1].line, std::uint32_t{1});
}
