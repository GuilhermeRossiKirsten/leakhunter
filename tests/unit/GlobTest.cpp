/// Glob matching. Worth its own file because a subtly wrong matcher does not
/// crash -- it silently suppresses the wrong leaks, which is the one failure
/// mode a leak detector must not have.

#include <chrono>
#include <string>

#include "TestFramework.hpp"
#include "leakhunter/core/Glob.hpp"

using leakhunter::core::globMatch;

LH_TEST(Glob, literals_match_exactly) {
    LH_CHECK(globMatch("abc", "abc"));
    LH_CHECK(!globMatch("abc", "abd"));
    LH_CHECK(!globMatch("abc", "ab"));
    LH_CHECK(!globMatch("abc", "abcd"));
}

LH_TEST(Glob, empty_pattern_matches_only_empty_text) {
    LH_CHECK(globMatch("", ""));
    LH_CHECK(!globMatch("", "a"));
}

LH_TEST(Glob, a_bare_star_matches_anything_including_nothing) {
    LH_CHECK(globMatch("*", ""));
    LH_CHECK(globMatch("*", "anything at all"));
    LH_CHECK(globMatch("**", "still fine"));
    LH_CHECK(globMatch("***", ""));
}

LH_TEST(Glob, question_mark_matches_exactly_one_character) {
    LH_CHECK(globMatch("a?c", "abc"));
    LH_CHECK(!globMatch("a?c", "ac"));
    LH_CHECK(!globMatch("a?c", "abbc"));
    LH_CHECK(globMatch("???", "xyz"));
    LH_CHECK(!globMatch("???", "xy"));
}

LH_TEST(Glob, stars_anchor_at_both_ends) {
    LH_CHECK(globMatch("abc*", "abcdef"));
    LH_CHECK(globMatch("*def", "abcdef"));
    LH_CHECK(globMatch("*cd*", "abcdef"));
    LH_CHECK(!globMatch("abc*", "xabcdef"));
    LH_CHECK(!globMatch("*def", "abcdefx"));
}

LH_TEST(Glob, backtracking_finds_a_late_match) {
    // The naive greedy match fails these; the matcher has to give the star back.
    LH_CHECK(globMatch("*x", "yyyx"));
    LH_CHECK(globMatch("*ab", "aab"));
    LH_CHECK(globMatch("a*b*c", "aXXbYYc"));
    LH_CHECK(globMatch("*a*a*a", "aaa"));
    LH_CHECK(!globMatch("*a*a*a*a", "aaa"));
}

LH_TEST(Glob, a_star_crosses_path_separators) {
    // Deliberate: `*/vendor/*` has to work without a `**` concept.
    LH_CHECK(globMatch("*/vendor/*", "/home/dev/app/vendor/parser/lex.cpp"));
    LH_CHECK(globMatch("*vendor*", "/a/b/c/vendor/d/e/f.cpp"));
    LH_CHECK(globMatch("/usr/*", "/usr/lib/x86_64-linux-gnu/libc.so.6"));
}

LH_TEST(Glob, matching_is_case_sensitive) {
    LH_CHECK(!globMatch("Abc", "abc"));
    LH_CHECK(!globMatch("*LIB*", "/usr/lib/libc.so"));
}

LH_TEST(Glob, realistic_symbol_patterns) {
    const std::string symbol = "(anonymous namespace)::allocateBuffer(unsigned long)";
    LH_CHECK(globMatch("*allocateBuffer*", symbol));
    LH_CHECK(globMatch("(anonymous namespace)::*", symbol));
    LH_CHECK(!globMatch("allocateBuffer*", symbol));  // not anchored at the start

    const std::string method = "mylib::detail::Cache::insert(int)";
    LH_CHECK(globMatch("mylib::detail::*", method));
    LH_CHECK(globMatch("mylib::*::Cache::*", method));
    LH_CHECK(!globMatch("mylib::detail2::*", method));
}

LH_TEST(Glob, a_pathological_pattern_stays_fast) {
    // The recursive formulation of this matcher is exponential here. The
    // iterative one is O(pattern * text). 30 stars against 60 characters would
    // not finish in a lifetime if this regressed, so the assertion is a clock.
    const std::string pattern = std::string(30, '*') + "b";
    const std::string text(60, 'a');

    const auto start = std::chrono::steady_clock::now();
    const bool matched = globMatch(pattern, text);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    LH_CHECK(!matched);
    LH_CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 100);
}
