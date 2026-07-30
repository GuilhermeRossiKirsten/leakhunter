/// @file SuppressionSet.hpp
/// @brief Rules for leaks that are known and accepted.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "leakhunter/core/Error.hpp"
#include "leakhunter/core/Types.hpp"

namespace leakhunter::analysis {

/// What a rule is matched against.
enum class SuppressionScope : std::uint8_t {
    Function = 0,  ///< the blamed frame's function name
    Module = 1,    ///< the blamed frame's object file
    File = 2,      ///< the blamed frame's source path
    Stack = 3,     ///< function, module or file of *any* frame in the stack
};

[[nodiscard]] std::string_view toString(SuppressionScope scope) noexcept;

/// One parsed rule, with enough provenance to tell the user which line of which
/// file is responsible when something goes wrong -- or when nothing does.
struct SuppressionRule {
    SuppressionScope scope = SuppressionScope::Function;
    std::string pattern;

    std::string origin;         ///< file the rule came from
    std::uint32_t line = 0;     ///< line within that file
    std::uint64_t hits = 0;     ///< leaks this rule suppressed

    /// Rendering for diagnostics: "leaks.supp:12: stack:*/vendor/*".
    [[nodiscard]] std::string describe() const;
};

/// A set of suppression rules loaded from one or more files.
///
/// Suppressed leaks are **counted and reported separately, never dropped
/// silently**. A leak detector that can be made quiet without saying so is a
/// leak detector nobody should trust: the whole value of the tool is that its
/// silence means something.
///
/// The rules are matched against the *blamed* frame by default. `stack:` widens
/// that to any frame, which is what actually solves the common case -- "I do
/// not care about anything allocated inside this vendored library" -- at the
/// cost of being easy to over-apply. Both are documented in docs/USAGE.md.
class SuppressionSet {
public:
    /// Parses @p path and appends its rules.
    ///
    /// A malformed line is an error rather than a warning: a typo in a
    /// suppression file silently un-suppresses (or over-suppresses) leaks, and
    /// finding that out from a CI run that passed for the wrong reason is worse
    /// than a startup failure.
    [[nodiscard]] Status loadFile(const std::filesystem::path& path);

    /// Parses rules from memory. @p origin is used only for diagnostics.
    [[nodiscard]] Status loadText(std::string_view text, const std::string& origin);

    /// @return the index of the rule that matched, or npos.
    [[nodiscard]] std::size_t match(const StackTrace& trace, std::size_t blamedFrame) const;

    /// Records a hit against a rule returned by match().
    void noteHit(std::size_t ruleIndex);

    [[nodiscard]] bool empty() const noexcept { return rules_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return rules_.size(); }
    [[nodiscard]] const std::vector<SuppressionRule>& rules() const noexcept { return rules_; }

    /// Rules that never matched anything. A rule that has rotted -- the
    /// function was renamed, the library was dropped -- is worse than no rule,
    /// because it looks like coverage that is not there.
    [[nodiscard]] std::vector<const SuppressionRule*> unusedRules() const;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    std::vector<SuppressionRule> rules_;
};

}  // namespace leakhunter::analysis
