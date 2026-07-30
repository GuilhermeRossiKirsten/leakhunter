/// @file SourceLineResolver.hpp
/// @brief DWARF-backed enrichment via llvm-symbolizer (or addr2line).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace leakhunter::symbols {

class SymbolResolver;

/// Batches addresses through an external symbolizer and feeds function names
/// and source locations back into a SymbolResolver.
///
/// This is not a nicety. dladdr() can only see the *dynamic* symbol table, so
/// every `static` function -- which is most application code -- comes back
/// nameless. DWARF is where those names actually live, and reading it is what
/// turns "<unknown> at 0x401234" into "allocateBuffer at buffer.cpp:42".
///
/// Strictly best effort: with no symbolizer installed, or a stripped binary,
/// the resolver keeps whatever dladdr() managed to provide.
class SourceLineResolver {
public:
    /// @param toolOverride explicit binary to use; empty means auto-detect.
    explicit SourceLineResolver(std::string toolOverride = {});

    /// @return true when a usable symbolizer was found.
    [[nodiscard]] bool available() const noexcept { return !tool_.empty(); }

    [[nodiscard]] const std::string& tool() const noexcept { return tool_; }

    /// Enriches every frame the resolver knows about. No-op when unavailable.
    /// @return number of program counters that gained a name or a location.
    std::size_t enrich(SymbolResolver& resolver) const;

private:
    struct Resolution {
        std::string function;
        std::string file;
        std::uint32_t line = 0;

        [[nodiscard]] bool empty() const noexcept { return function.empty() && line == 0; }
    };

    [[nodiscard]] std::vector<Resolution> query(const std::string& module,
                                                const std::vector<std::uint64_t>& addresses) const;

    std::string tool_;
    bool isLlvmSymbolizer_ = false;
};

}  // namespace leakhunter::symbols
