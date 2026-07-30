#include "leakhunter/symbols/SymbolResolver.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#define LEAKHUNTER_HAS_CXA_DEMANGLE 1
#else
#define LEAKHUNTER_HAS_CXA_DEMANGLE 0
#endif

namespace leakhunter::symbols {

StackTrace ISymbolResolver::resolveAll(const std::vector<std::uint64_t>& programCounters) const {
    StackTrace trace;
    trace.reserve(programCounters.size());
    for (const std::uint64_t pc : programCounters) {
        trace.push_back(resolve(pc));
    }
    return trace;
}

std::string SymbolResolver::demangle(const std::string& mangledName) {
    if (mangledName.empty()) {
        return mangledName;
    }

#if LEAKHUNTER_HAS_CXA_DEMANGLE
    // Only Itanium-mangled C++ names start with "_Z"; skipping the call for
    // plain C symbols avoids a malloc per frame.
    if (mangledName.rfind("_Z", 0) != 0) {
        return mangledName;
    }

    int status = 0;
    char* demangled = abi::__cxa_demangle(mangledName.c_str(), nullptr, nullptr, &status);
    if (status != 0 || demangled == nullptr) {
        std::free(demangled);
        return mangledName;
    }

    std::string result(demangled);
    std::free(demangled);
    return result;
#else
    return mangledName;
#endif
}

void SymbolResolver::addSymbol(tracker::RawSymbol&& symbol) {
    Entry entry;
    entry.moduleBase = symbol.moduleBase;
    entry.symbolAddress = symbol.symbolAddress;
    entry.function = demangle(symbol.mangledFunction);
    entry.module = std::move(symbol.module);

    symbols_.insert_or_assign(symbol.programCounter, std::move(entry));
}

void SymbolResolver::addModule(tracker::ModuleRange&& module) {
    if (module.path.empty() || module.span == 0) {
        return;
    }

    // The agent emits the map twice (start-up and shutdown); ignore repeats.
    const auto existing = std::find_if(
        modules_.begin(), modules_.end(), [&module](const tracker::ModuleRange& known) {
            return known.base == module.base && known.path == module.path;
        });
    if (existing != modules_.end()) {
        return;
    }

    const auto position =
        std::lower_bound(modules_.begin(), modules_.end(), module.base,
                         [](const tracker::ModuleRange& known, std::uint64_t base) {
                             return known.base < base;
                         });
    modules_.insert(position, std::move(module));
}

const tracker::ModuleRange* SymbolResolver::findModule(std::uint64_t programCounter) const {
    // First module whose base is greater than the address; the candidate is
    // the one before it.
    const auto upper = std::upper_bound(modules_.begin(), modules_.end(), programCounter,
                                        [](std::uint64_t address,
                                           const tracker::ModuleRange& known) {
                                            return address < known.base;
                                        });
    if (upper == modules_.begin()) {
        return nullptr;
    }

    const tracker::ModuleRange& candidate = *(upper - 1);
    return programCounter - candidate.base < candidate.span ? &candidate : nullptr;
}

void SymbolResolver::observe(std::uint64_t programCounter) {
    if (programCounter == 0 || symbols_.contains(programCounter)) {
        return;
    }

    const tracker::ModuleRange* module = findModule(programCounter);
    if (module == nullptr) {
        return;  // outside every known object: nothing useful can be said
    }

    Entry entry;
    entry.moduleBase = module->base;
    entry.module = module->path;
    symbols_.emplace(programCounter, std::move(entry));
}

void SymbolResolver::addResolution(std::uint64_t programCounter, const std::string& function,
                                   std::string file, std::uint32_t line) {
    const auto it = symbols_.find(programCounter);
    if (it == symbols_.end()) {
        return;
    }

    if (it->second.function.empty() && !function.empty() && function != "??") {
        it->second.function = demangle(function);
    }
    if (!file.empty() && line > 0) {
        it->second.file = std::move(file);
        it->second.line = line;
    }
}

bool SymbolResolver::needsFunctionName(std::uint64_t programCounter) const {
    const auto it = symbols_.find(programCounter);
    return it != symbols_.end() && it->second.function.empty();
}

StackFrame SymbolResolver::resolve(std::uint64_t programCounter) const {
    StackFrame frame;
    frame.address = programCounter;

    const auto it = symbols_.find(programCounter);
    if (it == symbols_.end()) {
        return frame;  // resolved == false; the report shows the raw address
    }

    const Entry& entry = it->second;
    frame.moduleBase = entry.moduleBase;
    frame.symbolAddress = entry.symbolAddress;
    frame.function = entry.function;
    frame.module = entry.module;
    frame.file = entry.file;
    frame.line = entry.line;
    frame.resolved = !entry.function.empty() || !entry.module.empty();
    return frame;
}

std::unordered_map<std::string, std::vector<std::uint64_t>>
SymbolResolver::programCountersByModule() const {
    std::unordered_map<std::string, std::vector<std::uint64_t>> grouped;

    for (const auto& [programCounter, entry] : symbols_) {
        if (entry.module.empty()) {
            continue;
        }
        grouped[entry.module].push_back(programCounter);
    }
    return grouped;
}

}  // namespace leakhunter::symbols
