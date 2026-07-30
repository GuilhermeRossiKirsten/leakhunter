#include "leakhunter/app/Application.hpp"

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "leakhunter/analysis/LeakAnalyzer.hpp"
#include "leakhunter/analysis/SuppressionSet.hpp"
#include "leakhunter/core/AgentLocator.hpp"
#include "leakhunter/report/DiagnosticsWriter.hpp"
#include "leakhunter/source/SourceSnippetReader.hpp"
#include "leakhunter/core/Logger.hpp"
#include "leakhunter/core/ScopedTempFile.hpp"
#include "leakhunter/ipc/TraceFormat.hpp"
#include "leakhunter/registry/AllocationRegistry.hpp"
#include "leakhunter/report/HtmlReportGenerator.hpp"
#include "leakhunter/report/JsonReportGenerator.hpp"
#include "leakhunter/symbols/SourceLineResolver.hpp"
#include "leakhunter/symbols/SymbolResolver.hpp"
#include "leakhunter/tracker/FileTraceSource.hpp"
#include "leakhunter/tracker/MemoryTracker.hpp"

namespace leakhunter::app {
namespace fs = std::filesystem;
namespace {

[[nodiscard]] std::string joinCommand(const std::vector<std::string>& command) {
    std::string joined;
    for (const std::string& part : command) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += part;
    }
    return joined;
}

}  // namespace

class Application::Impl {
public:
    Impl(std::unique_ptr<process::IProcessRunner> runner, std::ostream& out, std::ostream& err)
        : runner_(std::move(runner)), out_(out), err_(err) {}

    ExitCode run(const cli::Options& options);

    [[nodiscard]] const analysis::LeakReport* lastReport() const noexcept {
        return report_.has_value() ? &report_.value() : nullptr;
    }

private:
    /// How many of the top leak sites get their source line shown. Small on
    /// purpose: a terminal summary that scrolls is a summary nobody reads.
    static constexpr std::size_t kSnippetsInSummary = 3;

    [[nodiscard]] Result<fs::path> resolveAgent(const cli::Options& options) const;
    [[nodiscard]] Status writeReports(const cli::Options& options,
                                      const analysis::LeakReport& report);
    void printSummary(const analysis::LeakReport& report, const cli::Options& options) const;
    void printSnippet(const SourceSnippet& snippet) const;

    std::unique_ptr<process::IProcessRunner> runner_;
    std::ostream& out_;
    std::ostream& err_;
    std::optional<analysis::LeakReport> report_;
};

Result<fs::path> Application::Impl::resolveAgent(const cli::Options& options) const {
    if (!options.agentLibrary.empty()) {
        std::error_code ec;
        if (!fs::exists(options.agentLibrary, ec)) {
            return Error{fmt::format("--agent '{}' does not exist", options.agentLibrary.string())};
        }
        return fs::absolute(options.agentLibrary, ec);
    }
    return core::AgentLocator::locate();
}

ExitCode Application::Impl::run(const cli::Options& options) {
    // 0. Anything that can be rejected without running the target, first. A
    //    typo in a suppression file must not cost the user a whole traced run.
    analysis::SuppressionSet suppressions;
    for (const fs::path& file : options.suppressionFiles) {
        if (Status loaded = suppressions.loadFile(file); !loaded) {
            err_ << "leakhunter: " << loaded.message() << '\n';
            return ExitCode::UsageError;
        }
    }
    if (!suppressions.empty()) {
        log::debug("loaded {} suppression rule(s)", suppressions.size());
    }

    // 1. Locate the code we are going to inject.
    auto agent = resolveAgent(options);
    if (!agent) {
        err_ << "leakhunter: " << agent.message() << '\n';
        return ExitCode::InternalError;
    }
    log::debug("agent: {}", agent.value().string());

    // 2. Decide where the intermediate trace goes. The RAII wrapper deletes it
    //    on every exit path unless --keep-trace was requested.
    core::ScopedTempFile trace = options.traceFile.empty()
                                     ? core::ScopedTempFile{"leakhunter"}
                                     : core::ScopedTempFile{options.traceFile, false};
    if (options.keepTrace) {
        log::info("keeping trace at {}", trace.path().string());
    }

    // 3. Run the target with the agent preloaded.
    process::ProcessSpec spec;
    spec.command = options.targetCommand;
    spec.preloadLibrary = agent.value();
    spec.environment[ipc::kEnvTraceFile] = trace.path().string();
    spec.environment[ipc::kEnvMaxFrames] = std::to_string(options.maxFrames);
    if (options.verbosity == log::Level::Verbose) {
        spec.environment[ipc::kEnvVerbose] = "1";
    }

    log::info("running: {}", joinCommand(options.targetCommand));
    auto processResult = runner_->run(spec);
    if (!processResult) {
        err_ << "leakhunter: " << processResult.message() << '\n';
        return ExitCode::LaunchError;
    }
    log::debug("target exited with code {} after {} ms", processResult.value().exitCode,
               processResult.value().durationMs);

    // 4. Replay the trace into the registry and the symbol table.
    registry::AllocationRegistry allocations;
    symbols::SymbolResolver resolver;
    tracker::MemoryTracker memoryTracker(allocations, resolver);
    tracker::FileTraceSource source(trace.path());

    if (Status consumed = memoryTracker.consume(source); !consumed) {
        err_ << "leakhunter: " << consumed.message() << '\n';

        // The reader cannot tell "never injected" from "died before anything
        // was flushed", but we can: we watched the process exit.
        if (processResult.value().terminatingSignal != 0) {
            err_ << "  the target was killed by signal " << processResult.value().terminatingSignal
                 << " before any data reached the trace.\n";
        } else if (processResult.value().exitCode != 0) {
            err_ << "  the target exited with code " << processResult.value().exitCode
                 << "; if it is dynamically linked, it may have exited before allocating.\n";
        }
        return ExitCode::InternalError;
    }

    // 4b. The trace must belong to the process we launched.
    //
    //     The agent already refuses to trace anything the target exec'd, so this
    //     should be unreachable. It stays because the failure it guards against
    //     is a report full of some other process's leaks, presented as fact --
    //     and a cheap assertion is worth more than the alternative of trusting
    //     that the environment reached the child intact.
    const std::uint64_t tracedPid = memoryTracker.traceInfo().pid;
    if (tracedPid != 0 && tracedPid != processResult.value().pid) {
        err_ << "leakhunter: the trace was written by pid " << tracedPid
             << " but the target was pid " << processResult.value().pid
             << ".\n  This report would describe a different process; refusing to write it.\n";
        return ExitCode::InternalError;
    }

    // 5. Optional DWARF pass for file:line.
    if (options.resolveSourceLocations) {
        const symbols::SourceLineResolver sourceResolver;
        if (sourceResolver.available()) {
            sourceResolver.enrich(resolver);
        }
    }

    // 6. Analyse.
    analysis::AnalyzerConfig analyzerConfig;
    analyzerConfig.minLeakSize = options.minLeakSize;
    analyzerConfig.includeRuntimeLeaks = options.includeRuntimeLeaks;
    analyzerConfig.detectMismatchedFrees = options.detectMismatchedFrees;
    analyzerConfig.suppressions = suppressions.empty() ? nullptr : &suppressions;

    analysis::MismatchCheck mismatchCheck = analysis::MismatchCheck::Active;
    if (!options.detectMismatchedFrees) {
        mismatchCheck = analysis::MismatchCheck::Disabled;
    } else if (!allocations.mismatchDetectionIsTrustworthy()) {
        mismatchCheck = analysis::MismatchCheck::Suppressed;
        log::warn("mismatched-free detection suppressed: the target uses `new` but our "
                  "`operator delete` never ran, so it must define its own. Every pairing we "
                  "could derive would be an artefact of that, not a bug in the target.");
    }

    analysis::LeakAnalyzer analyzer(resolver, analyzerConfig);
    // takeMismatchedFrees() must run before stats() is read: it is what clears
    // the counter when the interposition turns out to be one-sided.
    std::vector<MismatchedFree> mismatches = allocations.takeMismatchedFrees();
    analysis::LeakReport report = analyzer.analyze(allocations.takeLiveAllocations(),
                                                   std::move(mismatches), allocations.stats());
    report.process = processResult.value();
    report.targetCommand = joinCommand(options.targetCommand);
    report.mismatchCheck = mismatchCheck;

    // 6b. Read the blamed lines out of the source tree.
    //
    //     After analysis, not during: the analyzer decides *which* frame is to
    //     blame and must stay free of I/O to keep being testable with a fake
    //     resolver and no files on disk.
    if (options.sourceSnippets) {
        source::SnippetConfig snippetConfig;
        snippetConfig.contextLines = options.snippetContext;
        snippetConfig.roots = options.sourceRoots;

        source::SourceSnippetReader snippets(snippetConfig);
        (void)snippets.enrich(report);

        if (!snippets.missingFiles().empty()) {
            // Worth one line: the usual cause is a report generated somewhere
            // other than where the target was built, and --source-root fixes it.
            log::info("{} source file(s) were not found, so those sites have no snippet; "
                      "pass --source-root <dir> if the tree lives elsewhere here",
                      snippets.missingFiles().size());
            for (const std::string& missing : snippets.missingFiles()) {
                log::debug("  no source at {}", missing);
            }
        }
        if (snippets.budgetExhausted()) {
            log::warn("the source-snippet size budget was reached; later sites have no snippet");
        }
    }

    // 7. Render.
    if (Status written = writeReports(options, report); !written) {
        err_ << "leakhunter: " << written.message() << '\n';
        return ExitCode::InternalError;
    }

    printSummary(report, options);

    // Compiler-style lines go to stderr, where a build tool expects diagnostics
    // and where they stay out of a `--json`-piped stdout.
    if (options.emitDiagnostics) {
        report::writeDiagnostics(report, err_, report::detectDiagnosticStyle());
    }

    if (options.keepTrace) {
        (void)trace.release();
    }

    report_ = std::move(report);

    // A rotted suppression rule is a correctness problem for the *gate*, not
    // for the program under test, so it gets the usage exit code rather than
    // LeaksFound -- and only when the user asked for that to be fatal.
    if (options.strictSuppressions && !report_->unusedRules.empty()) {
        err_ << "leakhunter: " << report_->unusedRules.size()
             << " suppression rule(s) matched nothing (--strict-suppressions)\n";
        for (const std::string& unused : report_->unusedRules) {
            err_ << "  " << unused << '\n';
        }
        return ExitCode::UsageError;
    }

    return report_->clean() ? ExitCode::Success : ExitCode::LeaksFound;
}

Status Application::Impl::writeReports(const cli::Options& options,
                                       const analysis::LeakReport& report) {
    std::error_code ec;
    fs::create_directories(options.outputDirectory, ec);
    if (ec && !fs::exists(options.outputDirectory)) {
        return Error{fmt::format("cannot create output directory '{}': {}",
                                 options.outputDirectory.string(), ec.message())};
    }

    std::vector<std::unique_ptr<report::IReportGenerator>> generators;
    if (options.emitJson) {
        generators.push_back(std::make_unique<report::JsonReportGenerator>());
    }
    if (options.emitHtml) {
        generators.push_back(std::make_unique<report::HtmlReportGenerator>());
    }

    for (const auto& generator : generators) {
        const fs::path path = options.outputDirectory / generator->defaultFileName();
        if (Status status = generator->generate(report, path); !status) {
            return status;
        }
    }
    return {};
}

void Application::Impl::printSnippet(const SourceSnippet& snippet) const {
    const std::size_t blamed = snippet.blamedIndex();
    if (snippet.empty() || blamed == static_cast<std::size_t>(-1)) {
        return;
    }

    // One line of code and one marker line, in the shape rustc and clang use --
    // familiar enough that nobody has to work out what they are looking at.
    const std::string& code = snippet.lines[blamed];
    const std::string gutter = fmt::format("{:>6}", snippet.blamedLine);

    out_ << fmt::format("                    {} | {}\n", gutter, code);

    // Point at the column when the symbolizer gave one. Columns are 1-based and
    // count the expanded line, which is why tabs are expanded at read time.
    std::string marker(gutter.size(), ' ');
    marker += " | ";
    if (snippet.column > 0 && snippet.column <= code.size() + 1) {
        marker.append(snippet.column - 1, ' ');
        marker += '^';
    } else {
        // No column: underline from the first non-blank character, so the marker
        // still says "this line" without claiming a precision we do not have.
        const std::size_t indent = code.find_first_not_of(" \t");
        marker.append(indent == std::string::npos ? 0 : indent, ' ');
        marker.append(indent == std::string::npos ? 1 : code.size() - indent, '~');
    }
    out_ << "                    " << marker << '\n';
}

void Application::Impl::printSummary(const analysis::LeakReport& report,
                                     const cli::Options& options) const {
    if (options.verbosity == log::Level::Quiet) {
        return;
    }

    const SessionStats& stats = report.stats;

    out_ << "\n";
    out_ << "  LeakHunter summary\n";
    out_ << "  ------------------------------------------------------------\n";
    out_ << fmt::format("  total allocations   {:>12}  ({})\n", stats.totalAllocations,
                        formatBytes(stats.totalBytesAllocated));
    out_ << fmt::format("  total freed         {:>12}  ({})\n", stats.totalDeallocations,
                        formatBytes(stats.totalBytesFreed));
    out_ << fmt::format("  peak live memory    {:>12}\n", formatBytes(stats.peakLiveBytes));
    out_ << fmt::format("  memory leaked       {:>12}\n", formatBytes(report.leakedBytes));
    out_ << fmt::format("  leaks               {:>12}  in {} distinct site(s)\n", report.leakCount,
                        report.groups.size());
    if (report.runtimeLeakCount > 0 && !options.includeRuntimeLeaks) {
        out_ << fmt::format("  runtime blocks      {:>12}  ({}, not listed; --include-runtime)\n",
                            report.runtimeLeakCount, formatBytes(report.runtimeLeakedBytes));
    }
    if (report.suppressedByRules > 0 || report.suppressedMismatchesByRules > 0) {
        // Both classes have to appear. A line reading "suppressed 1" while four
        // mismatched frees were also hidden is the quiet this feature must not
        // have.
        std::string what = fmt::format("{} leak(s)", report.suppressedByRules);
        if (report.suppressedMismatchesByRules > 0) {
            what += fmt::format(" + {} mismatch(es)", report.suppressedMismatchesByRules);
        }
        out_ << fmt::format("  suppressed          {:>12}  ({}, {} leaked, by {} rule(s))\n",
                            report.suppressedByRules + report.suppressedMismatchesByRules, what,
                            formatBytes(report.suppressedByRulesBytes), report.ruleHits.size());
    }
    if (stats.mismatchedFrees > 0) {
        out_ << fmt::format("  mismatched frees    {:>12}  (undefined behaviour)\n",
                            stats.mismatchedFrees);
    } else if (report.mismatchCheck == analysis::MismatchCheck::Suppressed) {
        // Saying nothing here would let "no mismatches listed" read as "no
        // mismatches", which is not what happened.
        out_ << "  mismatched frees         not checked  (target defines its own operator "
                "new/delete)\n";
    }
    out_ << "\n";

    if (!report.mismatchedFrees.empty()) {
        out_ << "  mismatched frees\n";
        const std::size_t shown = std::min<std::size_t>(report.mismatchedFrees.size(), 3);
        for (std::size_t i = 0; i < shown; ++i) {
            const MismatchedFree& mismatch = report.mismatchedFrees[i];
            const StackFrame* frame = mismatch.responsible();
            out_ << fmt::format("    {} allocated with {}, released with {}\n",
                                formatBytes(mismatch.size),
                                toSourceSpelling(mismatch.allocatedBy),
                                toSourceSpelling(mismatch.releasedBy));
            out_ << fmt::format("                        allocated at {}\n",
                                frame != nullptr ? frame->describe() : "<unknown>");
        }
        if (report.mismatchedFrees.size() > shown) {
            out_ << fmt::format("    ... and {} more (see the report)\n",
                                report.mismatchedFrees.size() - shown);
        }
        out_ << "\n";
    }

    if (!report.groups.empty()) {
        out_ << "  top leak sites\n";
        const std::size_t shown = std::min<std::size_t>(report.groups.size(), 5);
        for (std::size_t i = 0; i < shown; ++i) {
            const analysis::LeakGroup& group = report.groups[i];
            out_ << fmt::format("    {:>10}  x{:<6} {}\n", formatBytes(group.totalBytes),
                                group.count, group.function);
            if (!group.location.empty()) {
                out_ << fmt::format("                        at {}\n", group.location);
            }

            // The source line itself, for the first few. This is the difference
            // between being told where to look and being shown.
            if (i < kSnippetsInSummary) {
                printSnippet(group.snippet);
            }
        }
        if (report.groups.size() > shown) {
            out_ << fmt::format("    ... and {} more (see the report)\n",
                                report.groups.size() - shown);
        }
        out_ << "\n";
    }

    if (options.emitJson) {
        out_ << fmt::format("  report: {}\n",
                            (options.outputDirectory / "report.json").string());
    }
    if (options.emitHtml) {
        out_ << fmt::format("  report: {}\n",
                            (options.outputDirectory / "report.html").string());
    }
    out_ << "\n";
}

// --- public facade ---------------------------------------------------------

Application::Application(std::unique_ptr<process::IProcessRunner> runner, std::ostream& out,
                         std::ostream& err)
    : impl_(std::make_unique<Impl>(std::move(runner), out, err)) {}

Application::~Application() = default;

ExitCode Application::run(const cli::Options& options) {
    return impl_->run(options);
}

const analysis::LeakReport* Application::lastReport() const noexcept {
    return impl_->lastReport();
}

}  // namespace leakhunter::app
