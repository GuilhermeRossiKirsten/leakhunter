#include "leakhunter/analysis/SuppressionSet.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <utility>

#include <fmt/format.h>

#include "leakhunter/core/Glob.hpp"

namespace leakhunter::analysis {
namespace {

constexpr std::array<std::pair<std::string_view, SuppressionScope>, 4> kScopeKeywords{{
    {"function", SuppressionScope::Function},
    {"module", SuppressionScope::Module},
    {"file", SuppressionScope::File},
    {"stack", SuppressionScope::Stack},
}};

[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\r'; };
    while (!text.empty() && !notSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && !notSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string knownScopes() {
    std::string joined;
    for (const auto& [keyword, _] : kScopeKeywords) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += keyword;
    }
    return joined;
}

/// True when @p frame matches @p pattern under a per-frame scope.
[[nodiscard]] bool frameMatches(const StackFrame& frame, SuppressionScope scope,
                                std::string_view pattern) {
    switch (scope) {
        case SuppressionScope::Function:
            return core::globMatch(pattern, frame.function);
        case SuppressionScope::Module:
            return core::globMatch(pattern, frame.module);
        case SuppressionScope::File:
            return core::globMatch(pattern, frame.file);
        case SuppressionScope::Stack:
            // Any of the three, so one `stack:` rule covers a library whether
            // its frames were resolved by name, by module or by source path.
            return core::globMatch(pattern, frame.function) ||
                   core::globMatch(pattern, frame.module) ||
                   core::globMatch(pattern, frame.file);
    }
    return false;
}

}  // namespace

std::string_view toString(SuppressionScope scope) noexcept {
    switch (scope) {
        case SuppressionScope::Function: return "function";
        case SuppressionScope::Module: return "module";
        case SuppressionScope::File: return "file";
        case SuppressionScope::Stack: return "stack";
    }
    return "unknown";
}

std::string SuppressionRule::describe() const {
    return fmt::format("{}:{}: {}:{}", origin, line, toString(scope), pattern);
}

Status SuppressionSet::loadText(std::string_view text, const std::string& origin) {
    std::uint32_t lineNumber = 0;
    std::size_t position = 0;

    while (position <= text.size()) {
        const std::size_t newline = text.find('\n', position);
        const std::string_view raw =
            text.substr(position, newline == std::string_view::npos ? std::string_view::npos
                                                                   : newline - position);
        position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++lineNumber;

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return Error{fmt::format(
                "{}:{}: expected '<scope>:<pattern>', got '{}'. Known scopes: {}.", origin,
                lineNumber, line, knownScopes())};
        }

        const std::string_view keyword = trim(line.substr(0, colon));
        const std::string_view pattern = trim(line.substr(colon + 1));

        if (pattern.empty()) {
            return Error{fmt::format("{}:{}: '{}' has an empty pattern; a rule matching "
                                     "everything is never what anyone means",
                                     origin, lineNumber, keyword)};
        }

        SuppressionScope scope{};
        bool recognised = false;
        for (const auto& [candidate, value] : kScopeKeywords) {
            if (keyword == candidate) {
                scope = value;
                recognised = true;
                break;
            }
        }
        if (!recognised) {
            return Error{fmt::format("{}:{}: unknown scope '{}'. Known scopes: {}.", origin,
                                     lineNumber, keyword, knownScopes())};
        }

        SuppressionRule rule;
        rule.scope = scope;
        rule.pattern = std::string{pattern};
        rule.origin = origin;
        rule.line = lineNumber;
        rules_.push_back(std::move(rule));
    }

    return {};
}

Status SuppressionSet::loadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Error{fmt::format("cannot read suppression file '{}'", path.string())};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return loadText(buffer.str(), path.string());
}

std::size_t SuppressionSet::match(const StackTrace& trace, std::size_t blamedFrame) const {
    for (std::size_t i = 0; i < rules_.size(); ++i) {
        const SuppressionRule& rule = rules_[i];

        if (rule.scope == SuppressionScope::Stack) {
            for (const StackFrame& frame : trace) {
                if (frameMatches(frame, rule.scope, rule.pattern)) {
                    return i;
                }
            }
            continue;
        }

        // Per-frame scopes look only at the frame the report blames, so a rule
        // written for one function cannot silently swallow its callers.
        if (blamedFrame < trace.size() &&
            frameMatches(trace[blamedFrame], rule.scope, rule.pattern)) {
            return i;
        }
    }
    return npos;
}

void SuppressionSet::noteHit(std::size_t ruleIndex) {
    if (ruleIndex < rules_.size()) {
        ++rules_[ruleIndex].hits;
    }
}

std::vector<const SuppressionRule*> SuppressionSet::unusedRules() const {
    std::vector<const SuppressionRule*> unused;
    for (const SuppressionRule& rule : rules_) {
        if (rule.hits == 0) {
            unused.push_back(&rule);
        }
    }
    return unused;
}

}  // namespace leakhunter::analysis
