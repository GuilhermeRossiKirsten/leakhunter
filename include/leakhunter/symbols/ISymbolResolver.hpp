/// @file ISymbolResolver.hpp
/// @brief Program counter -> human readable frame.

#pragma once

#include <cstdint>
#include <vector>

#include "leakhunter/core/Types.hpp"

namespace leakhunter::symbols {

class ISymbolResolver {
public:
    virtual ~ISymbolResolver() = default;

    /// Never fails: an unknown address yields a frame with `resolved == false`
    /// so reports still show the raw pointer instead of dropping the frame.
    [[nodiscard]] virtual StackFrame resolve(std::uint64_t programCounter) const = 0;

    [[nodiscard]] virtual StackTrace resolveAll(
        const std::vector<std::uint64_t>& programCounters) const;
};

}  // namespace leakhunter::symbols
