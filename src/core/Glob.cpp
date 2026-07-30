#include "leakhunter/core/Glob.hpp"

namespace leakhunter::core {

bool globMatch(std::string_view pattern, std::string_view text) noexcept {
    constexpr std::size_t kNoStar = static_cast<std::size_t>(-1);

    std::size_t patternIndex = 0;
    std::size_t textIndex = 0;

    // Position of the most recent `*` in the pattern, and how much of the text
    // it was assumed to consume. On a mismatch we come back here and let it
    // consume one more character, which is what makes this linear rather than
    // exponential: each `*` is retried by advancing, never by recursing.
    std::size_t lastStar = kNoStar;
    std::size_t textAtStar = 0;

    while (textIndex < text.size()) {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' || pattern[patternIndex] == text[textIndex])) {
            ++patternIndex;
            ++textIndex;
        } else if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
            lastStar = patternIndex;
            ++patternIndex;
            textAtStar = textIndex;
        } else if (lastStar != kNoStar) {
            patternIndex = lastStar + 1;
            ++textAtStar;
            textIndex = textAtStar;
        } else {
            return false;
        }
    }

    // Text exhausted: the rest of the pattern may only be `*`s.
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

}  // namespace leakhunter::core
