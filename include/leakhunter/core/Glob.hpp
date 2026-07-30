/// @file Glob.hpp
/// @brief Minimal glob matching, for suppression patterns.

#pragma once

#include <string_view>

namespace leakhunter::core {

/// Matches @p text against a glob @p pattern.
///
/// Supported: `*` (any sequence, **including** `/`) and `?` (exactly one
/// character). Everything else is literal, and matching is case-sensitive
/// because the things being matched are Linux paths and C++ symbol names.
///
/// There are deliberately no character classes, no brace expansion and no
/// distinction between `*` and `**`. A suppression file is read by a human and
/// matched against a few thousand strings; the useful patterns are all of the
/// form `*/vendor/*` or `mylib::detail::*`, and every feature beyond that buys
/// more confusion than power.
///
/// The implementation backtracks at each `*` rather than recursing, so it is
/// O(pattern * text) in the worst case. A pattern like `*a*a*a*a*b` cannot make
/// it blow up exponentially, which matters when the patterns come from a file
/// someone else wrote.
[[nodiscard]] bool globMatch(std::string_view pattern, std::string_view text) noexcept;

}  // namespace leakhunter::core
