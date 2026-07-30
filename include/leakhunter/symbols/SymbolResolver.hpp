/// @file SymbolResolver.hpp
/// @brief Turns raw program counters into named, optionally located frames.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "leakhunter/symbols/ISymbolResolver.hpp"
#include "leakhunter/tracker/ITraceSource.hpp"

namespace leakhunter::symbols {

/// Symbolisation happens in two layers:
///
///  1. dladdr(), performed by the agent inside the target process during
///     shutdown and shipped in the trace. This always works and gives us
///     function + module, but never file:line.
///  2. An optional source-location pass on the host (see SourceLineResolver),
///     which reads DWARF from the same binaries via llvm-symbolizer.
///
/// Layer 1 is the contract; layer 2 is a best-effort enrichment that silently
/// degrades when debug info or the symbolizer is unavailable.
class SymbolResolver final : public ISymbolResolver {
public:
    SymbolResolver() = default;

    /// Ingests one dladdr() result from the trace.
    void addSymbol(tracker::RawSymbol&& symbol);

    /// Ingests one loaded object's address range.
    void addModule(tracker::ModuleRange&& module);

    /// Registers a program counter seen in a stack trace.
    ///
    /// Normally redundant -- the agent's dladdr pass already described every
    /// call site. It matters when that pass never ran, which is exactly the
    /// case for a target that crashed: the module map still lets us place the
    /// address inside an object, and the DWARF pass does the rest.
    void observe(std::uint64_t programCounter);

    /// Applies information gathered by a SourceLineResolver.
    ///
    /// @p function overwrites nothing unless the existing name is empty: a
    /// dladdr() name is the ground truth, DWARF only fills the gaps. That
    /// matters because dladdr cannot see static functions -- they never reach
    /// the dynamic symbol table -- so most application frames arrive nameless
    /// and are named here.
    void addResolution(std::uint64_t programCounter, const std::string& function,
                       std::string file, std::uint32_t line, std::uint32_t column = 0);

    /// True when @p programCounter still has no function name.
    [[nodiscard]] bool needsFunctionName(std::uint64_t programCounter) const;

    [[nodiscard]] StackFrame resolve(std::uint64_t programCounter) const override;

    [[nodiscard]] std::size_t size() const noexcept { return symbols_.size(); }

    /// Every (module, moduleOffset) pair known so far, grouped by module. Feeds
    /// the source-location pass without exposing the internal storage.
    [[nodiscard]] std::unordered_map<std::string, std::vector<std::uint64_t>>
    programCountersByModule() const;

    /// Demangles an Itanium ABI name, returning it unchanged when it is not
    /// mangled (plain C functions) or when demangling fails.
    [[nodiscard]] static std::string demangle(const std::string& mangledName);

private:
    struct Entry {
        std::uint64_t moduleBase = 0;
        std::uint64_t symbolAddress = 0;
        std::string function;  ///< already demangled
        std::string module;
        std::string file;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
    };

    /// Finds the loaded object containing @p programCounter, or nullptr.
    [[nodiscard]] const tracker::ModuleRange* findModule(std::uint64_t programCounter) const;

    std::unordered_map<std::uint64_t, Entry> symbols_;

    /// Kept sorted by base address so lookups are a binary search. Small (tens
    /// of entries), so rebuilding the order on insert is cheaper than a tree.
    std::vector<tracker::ModuleRange> modules_;
};

}  // namespace leakhunter::symbols
