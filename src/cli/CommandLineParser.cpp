#include "leakhunter/cli/CommandLineParser.hpp"

#include <charconv>
#include <ostream>

#include <fmt/format.h>

#include "leakhunter/core/BuildConfig.hpp"
#include "leakhunter/ipc/TraceFormat.hpp"

namespace leakhunter::cli {
namespace {

/// Parses an unsigned decimal, rejecting trailing garbage ("32k" is an error,
/// not 32) so typos surface immediately.
template <typename T>
[[nodiscard]] bool parseUnsigned(std::string_view text, T& out) {
    if (text.empty()) {
        return false;
    }
    T value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool isOption(std::string_view token) {
    return token.size() >= 2 && token[0] == '-';
}

}  // namespace

CommandLineParser::CommandLineParser(std::ostream& out) : out_(out) {}

void CommandLineParser::printUsage() const {
    out_ << R"(leakhunter -- lightweight memory leak detector for C++ applications

USAGE
    leakhunter [options] <program> [program-args...]
    leakhunter [options] -- <program> [program-args...]

    The first non-option token starts the monitored command; every token after
    it is forwarded to that program untouched.

OPTIONS
    -o, --output <dir>     Directory for the generated reports
                           (default: leakhunter-report)
        --html             Generate report.html only
        --json             Generate report.json only
                           (default: generate both)
        --max-frames <n>   Stack frames captured per allocation (default: 32,
                           max: 128). Lower values reduce overhead.
        --min-leak-size <n>
                           Omit leaks smaller than <n> bytes from the listing
        --include-runtime  Also list blocks the C runtime never frees by design
                           (stdio buffers, locale tables). Off by default: they
                           are expected, and reporting them buries real leaks.
        --suppressions <f> Ignore leaks and mismatched frees matching the rules
                           in <f>. Repeatable. Suppressed findings are counted
                           and listed separately, never dropped silently.
                           Format: docs/USAGE.md.
        --strict-suppressions
                           Exit 2 if any suppression rule matched nothing
        --no-source        Skip file:line resolution (do not run llvm-symbolizer).
                           Implies --no-source-snippets: with no file:line there
                           is nothing to read.
        --no-source-snippets
                           Do not read source files. By default the reports embed
                           the blamed lines, which means report.html contains
                           excerpts of your code -- turn this off if the report
                           is going somewhere the source should not.
        --snippet-context <n>
                           Lines of source context on each side (default: 4,
                           max: 32)
        --source-root <dir>
                           Where to look for sources whose recorded path does not
                           exist here, e.g. a report generated on a different
                           machine from the build. Repeatable.
        --diagnostics      Also write compiler-style findings to stderr, so an
                           editor can jump to them. Becomes GitHub Actions
                           annotations automatically when $GITHUB_ACTIONS is set.
        --no-mismatch-check
                           Do not report blocks released through the wrong entry
                           point (`new[]` freed with `delete`). The check needs
                           both halves of the C++ pair interposed; a program
                           that defines its own global operator new or delete
                           can defeat it.
        --keep-trace       Keep the intermediate binary trace file
        --trace-file <p>   Write the intermediate trace to <p> (implies --keep-trace)
        --agent <path>     Use a specific libleakhunter_agent.so
    -v, --verbose          Verbose diagnostics on stderr
    -q, --quiet            Errors only
    -h, --help             Show this help and exit
    -V, --version          Show version and exit

EXIT CODES
    0  target ran, nothing found
    1  target ran, leaks and/or mismatched frees found
    2  invalid arguments
    3  the target could not be started
    4  internal error (tracing or report generation failed)

EXAMPLES
    leakhunter ./app
    leakhunter --html ./app
    leakhunter --json ./app
    leakhunter --output reports ./app
    leakhunter --verbose ./app
    leakhunter --max-frames 64 -o reports -- ./app --config prod.ini
)";
}

void CommandLineParser::printVersion() const {
    out_ << fmt::format("leakhunter {} ({} {})\n", build::kVersion, build::kCompilerId,
                        build::kCompilerVersion);
}

Result<Options> CommandLineParser::parse(int argc, char** argv) const {
    std::vector<std::string_view> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return parse(args);
}

Result<Options> CommandLineParser::parse(std::span<const std::string_view> args) const {
    Options options;
    bool formatSelected = false;

    std::size_t index = 0;
    for (; index < args.size(); ++index) {
        const std::string_view token = args[index];

        if (token == "--") {
            ++index;
            break;
        }
        if (!isOption(token)) {
            break;  // start of the monitored command
        }

        // Helper for options taking a value.
        const auto takeValue = [&](std::string_view name) -> Result<std::string_view> {
            if (index + 1 >= args.size()) {
                return Error{fmt::format("option '{}' requires a value", name)};
            }
            return args[++index];
        };

        if (token == "-h" || token == "--help") {
            printUsage();
            options.shouldExitEarly = true;
            options.earlyExitCode = 0;
            return options;
        }
        if (token == "-V" || token == "--version") {
            printVersion();
            options.shouldExitEarly = true;
            options.earlyExitCode = 0;
            return options;
        }
        if (token == "-v" || token == "--verbose") {
            options.verbosity = log::Level::Verbose;
        } else if (token == "-q" || token == "--quiet") {
            options.verbosity = log::Level::Quiet;
        } else if (token == "--html") {
            options.emitHtml = true;
            options.emitJson = formatSelected && options.emitJson;
            formatSelected = true;
        } else if (token == "--json") {
            options.emitJson = true;
            options.emitHtml = formatSelected && options.emitHtml;
            formatSelected = true;
        } else if (token == "--no-source") {
            options.resolveSourceLocations = false;
            // Without file:line there is nothing to open, so the snippets go
            // too. Making that implicit here means the two flags can never be
            // left in a combination that promises source and cannot deliver it.
            options.sourceSnippets = false;
        } else if (token == "--no-source-snippets") {
            options.sourceSnippets = false;
        } else if (token == "--snippet-context") {
            auto value = takeValue(token);
            if (!value) return value.error();
            std::uint32_t context = 0;
            if (!parseUnsigned(value.value(), context) || context > 32) {
                return Error{fmt::format("--snippet-context expects 0..32, got '{}'",
                                         value.value())};
            }
            options.snippetContext = context;
        } else if (token == "--source-root") {
            auto value = takeValue(token);
            if (!value) return value.error();
            options.sourceRoots.emplace_back(std::string{value.value()});
        } else if (token == "--diagnostics") {
            options.emitDiagnostics = true;
        } else if (token == "--no-mismatch-check") {
            options.detectMismatchedFrees = false;
        } else if (token == "--suppressions") {
            auto value = takeValue(token);
            if (!value) return value.error();
            options.suppressionFiles.emplace_back(std::string{value.value()});
        } else if (token == "--strict-suppressions") {
            options.strictSuppressions = true;
        } else if (token == "--include-runtime") {
            options.includeRuntimeLeaks = true;
        } else if (token == "--keep-trace") {
            options.keepTrace = true;
        } else if (token == "-o" || token == "--output") {
            auto value = takeValue(token);
            if (!value) return value.error();
            options.outputDirectory = std::string{value.value()};
        } else if (token == "--trace-file") {
            auto value = takeValue(token);
            if (!value) return value.error();
            options.traceFile = std::string{value.value()};
            options.keepTrace = true;
        } else if (token == "--agent") {
            auto value = takeValue(token);
            if (!value) return value.error();
            options.agentLibrary = std::string{value.value()};
        } else if (token == "--max-frames") {
            auto value = takeValue(token);
            if (!value) return value.error();
            std::uint16_t frames = 0;
            if (!parseUnsigned(value.value(), frames) || frames == 0) {
                return Error{fmt::format("--max-frames expects a positive integer, got '{}'",
                                         value.value())};
            }
            if (frames > ipc::kMaxFrames) {
                return Error{fmt::format("--max-frames cannot exceed {}", ipc::kMaxFrames)};
            }
            options.maxFrames = frames;
        } else if (token == "--min-leak-size") {
            auto value = takeValue(token);
            if (!value) return value.error();
            if (!parseUnsigned(value.value(), options.minLeakSize)) {
                return Error{
                    fmt::format("--min-leak-size expects an integer, got '{}'", value.value())};
            }
        } else {
            return Error{fmt::format("unknown option '{}' (try --help)", token)};
        }
    }

    for (; index < args.size(); ++index) {
        options.targetCommand.emplace_back(args[index]);
    }

    if (options.targetCommand.empty()) {
        return Error{"no program specified (try --help)"};
    }
    if (!options.emitHtml && !options.emitJson) {
        return Error{"no report format selected"};
    }

    return options;
}

}  // namespace leakhunter::cli
