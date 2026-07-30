#include "leakhunter/symbols/SourceLineResolver.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "leakhunter/core/Logger.hpp"
#include "leakhunter/symbols/SymbolResolver.hpp"

namespace leakhunter::symbols {
namespace fs = std::filesystem;
namespace {

/// Addresses per symbolizer invocation. Keeps the command line well under
/// ARG_MAX while amortising process start-up over many lookups.
constexpr std::size_t kBatchSize = 256;

/// Candidates in preference order. llvm-symbolizer handles split debug info
/// and inlining better than addr2line, so it wins when both are present.
constexpr std::array<std::string_view, 8> kCandidateTools{
    "llvm-symbolizer", "llvm-symbolizer-19", "llvm-symbolizer-18", "llvm-symbolizer-17",
    "llvm-symbolizer-16", "llvm-symbolizer-15", "llvm-symbolizer-14", "addr2line",
};

[[nodiscard]] bool isOnPath(std::string_view tool) {
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) {
        return false;
    }

    std::stringstream stream{pathEnv};
    std::string directory;
    while (std::getline(stream, directory, ':')) {
        if (directory.empty()) {
            continue;
        }
        std::error_code ec;
        const fs::path candidate = fs::path{directory} / tool;
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
            return true;
        }
    }
    return false;
}

/// Wraps a path for /bin/sh. Single quotes disable every expansion, and an
/// embedded quote is closed, escaped, and reopened.
[[nodiscard]] std::string shellQuote(std::string_view text) {
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('\'');
    for (const char character : text) {
        if (character == '\'') {
            quoted += R"('\'')";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

/// True for a line the symbolizer wrote to complain rather than to answer.
///
/// This check exists because its absence hid a real bug for a long time: the
/// invocation used `--inlines=false --demangle=false`, which llvm-symbolizer
/// rejects outright ("unknown argument"). stderr went to /dev/null, the output
/// was empty, and every frame silently fell back to what dladdr could see --
/// which is nothing for a `static` function. The tool's headline capability was
/// broken on exactly the machines that had the preferred symbolizer installed.
[[nodiscard]] bool looksLikeDiagnostic(std::string_view line) {
    return line.starts_with("error:") || line.starts_with("warning:") ||
           line.find("unknown argument") != std::string_view::npos ||
           line.find("Unknown command line argument") != std::string_view::npos;
}

[[nodiscard]] std::vector<std::string> runCommand(const std::string& command) {
    std::vector<std::string> lines;

    // NOLINTNEXTLINE(cert-env33-c) -- built from a fixed tool name plus
    // shell-quoted arguments; no user-controlled shell text reaches this.
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return lines;
    }

    std::string current;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        current.assign(buffer.data());
        while (!current.empty() && (current.back() == '\n' || current.back() == '\r')) {
            current.pop_back();
        }
        if (!current.empty()) {
            lines.push_back(current);  // blank record separators carry no data
        }
    }

    ::pclose(pipe);
    return lines;
}

/// Parses "path/to/file.cpp:42" and "path/to/file.cpp:42:7".
///
/// The column is kept, not discarded: it is what lets a report point a caret at
/// the exact call rather than underlining a whole line. addr2line does not emit
/// one, so @p column stays 0 there and callers fall back to the whole line.
[[nodiscard]] bool parseLocation(std::string_view text, std::string& file, std::uint32_t& line,
                                 std::uint32_t& column) {
    if (text.empty() || text.starts_with("??")) {
        return false;
    }

    const std::size_t lastColon = text.rfind(':');
    if (lastColon == std::string_view::npos) {
        return false;
    }

    std::string_view head = text.substr(0, lastColon);
    std::string_view tail = text.substr(lastColon + 1);

    std::uint32_t parsed = 0;
    auto [ptr, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), parsed);
    if (ec != std::errc{} || ptr != tail.data() + tail.size()) {
        return false;
    }

    // Two numeric tails means "file:line:column": the one just parsed is the
    // column, and the line is the field before it.
    std::uint32_t parsedColumn = 0;
    if (const std::size_t secondColon = head.rfind(':'); secondColon != std::string_view::npos) {
        const std::string_view maybeLine = head.substr(secondColon + 1);
        std::uint32_t lineCandidate = 0;
        auto [p2, e2] =
            std::from_chars(maybeLine.data(), maybeLine.data() + maybeLine.size(), lineCandidate);
        if (e2 == std::errc{} && p2 == maybeLine.data() + maybeLine.size()) {
            head = head.substr(0, secondColon);
            parsedColumn = parsed;
            parsed = lineCandidate;
        }
    }

    if (head.empty() || head == "??" || parsed == 0) {
        return false;
    }

    file.assign(head);
    line = parsed;
    column = parsedColumn;
    return true;
}

}  // namespace

SourceLineResolver::SourceLineResolver(std::string toolOverride) {
    if (!toolOverride.empty()) {
        tool_ = std::move(toolOverride);
    } else if (const char* fromEnv = std::getenv("LEAKHUNTER_SYMBOLIZER");
               fromEnv != nullptr && *fromEnv != '\0') {
        tool_ = fromEnv;
    } else {
        for (const std::string_view candidate : kCandidateTools) {
            if (isOnPath(candidate)) {
                tool_.assign(candidate);
                break;
            }
        }
    }

    if (tool_.empty()) {
        log::debug("no symbolizer on PATH; frames will show only what dladdr provided");
        return;
    }

    isLlvmSymbolizer_ = tool_.find("llvm-symbolizer") != std::string::npos;
    log::debug("symbolizing with '{}'", tool_);
}

std::vector<SourceLineResolver::Resolution> SourceLineResolver::query(
    const std::string& module, const std::vector<std::uint64_t>& addresses) const {
    std::vector<Resolution> results(addresses.size());

    for (std::size_t start = 0; start < addresses.size(); start += kBatchSize) {
        const std::size_t stop = std::min(start + kBatchSize, addresses.size());

        std::string command = tool_;
        if (isLlvmSymbolizer_) {
            // Disabling inlining is load-bearing: it is what keeps the output to
            // exactly two lines per address, which the pairing below relies on.
            //
            // Both spellings here are the documented *aliases*, chosen on
            // purpose. The canonical modern forms are `--no-inlines` and
            // `--no-demangle`, but the aliases exist for backward compatibility
            // and so work across the llvm-symbolizer versions in kCandidateTools.
            // `--inlines=false` and `--demangle=false`, which this used to send,
            // are accepted by neither -- see looksLikeDiagnostic above.
            //
            // Names stay mangled so demangling happens in one place:
            // SymbolResolver::demangle.
            command += fmt::format(
                " --obj={} --functions=linkage --inlining=false -demangle=false"
                " --output-style=LLVM",
                shellQuote(module));
        } else {
            command += fmt::format(" -e {} -f", shellQuote(module));
        }

        for (std::size_t i = start; i < stop; ++i) {
            command += fmt::format(" 0x{:x}", addresses[i]);
        }

        // stderr is merged in rather than discarded. Discarding it is how a
        // broken invocation looked exactly like a stripped binary.
        command += " 2>&1";

        // Both tools emit two non-empty lines per address: the function name,
        // then the source location.
        std::vector<std::string> output = runCommand(command);

        const auto firstDiagnostic =
            std::find_if(output.begin(), output.end(),
                         [](const std::string& line) { return looksLikeDiagnostic(line); });
        if (firstDiagnostic != output.end()) {
            // Do not try to salvage a partial answer: the line pairing would be
            // off by however many diagnostics appeared, and quietly mis-pairing
            // names to locations is worse than having none.
            log::warn("'{}' rejected our invocation: {} -- frames will only have what dladdr "
                      "provided. Please report this with your symbolizer version.",
                      tool_, *firstDiagnostic);
            return results;
        }

        for (std::size_t i = start; i < stop; ++i) {
            const std::size_t functionIndex = (i - start) * 2;
            if (functionIndex + 1 >= output.size()) {
                break;
            }

            const std::string& name = output[functionIndex];
            if (name != "??") {
                results[i].function = name;
            }
            // A missing location is normal (no debug info); the name alone is
            // still worth keeping.
            (void)parseLocation(output[functionIndex + 1], results[i].file, results[i].line,
                                results[i].column);
        }
    }

    return results;
}

std::size_t SourceLineResolver::enrich(SymbolResolver& resolver) const {
    if (!available()) {
        return 0;
    }

    std::size_t enriched = 0;

    for (const auto& [module, programCounters] : resolver.programCountersByModule()) {
        std::error_code ec;
        if (module.empty() || !fs::exists(module, ec)) {
            continue;
        }

        std::vector<std::uint64_t> offsets;
        offsets.reserve(programCounters.size());
        for (const std::uint64_t programCounter : programCounters) {
            offsets.push_back(resolver.resolve(programCounter).moduleOffset());
        }

        std::vector<Resolution> resolutions = query(module, offsets);

        // Module-relative offsets are right for position-independent objects.
        // A non-PIE executable is mapped at its link address, so there the
        // absolute address is what the symbolizer wants. Rather than guessing
        // which kind of object this is, retry the ones that produced nothing.
        std::vector<std::size_t> unresolved;
        for (std::size_t i = 0; i < resolutions.size(); ++i) {
            if (resolutions[i].empty() && programCounters[i] != offsets[i]) {
                unresolved.push_back(i);
            }
        }

        if (!unresolved.empty()) {
            std::vector<std::uint64_t> absolute;
            absolute.reserve(unresolved.size());
            for (const std::size_t index : unresolved) {
                absolute.push_back(programCounters[index]);
            }

            const std::vector<Resolution> retry = query(module, absolute);
            for (std::size_t i = 0; i < retry.size(); ++i) {
                if (!retry[i].empty()) {
                    resolutions[unresolved[i]] = retry[i];
                }
            }
        }

        for (std::size_t i = 0; i < resolutions.size() && i < programCounters.size(); ++i) {
            if (resolutions[i].empty()) {
                continue;
            }
            resolver.addResolution(programCounters[i], resolutions[i].function,
                                   resolutions[i].file, resolutions[i].line,
                                   resolutions[i].column);
            ++enriched;
        }
    }

    if (enriched == 0) {
        // The observable symptom of a symbolizer that is present but unusable.
        // Saying so beats letting the user conclude their binary is stripped.
        log::warn("'{}' resolved nothing; every frame will show only what dladdr provided. "
                  "A stripped target explains this -- so does a symbolizer that rejected our "
                  "arguments (run with --verbose).",
                  tool_);
    } else {
        log::debug("enriched {} call sites with '{}'", enriched, tool_);
    }
    return enriched;
}

}  // namespace leakhunter::symbols
