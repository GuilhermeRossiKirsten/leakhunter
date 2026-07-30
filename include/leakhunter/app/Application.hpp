/// @file Application.hpp
/// @brief Orchestration layer -- wires the modules together, owns no policy.

#pragma once

#include <iosfwd>
#include <memory>

#include "leakhunter/analysis/LeakReport.hpp"
#include "leakhunter/cli/Options.hpp"
#include "leakhunter/core/Error.hpp"
#include "leakhunter/process/IProcessRunner.hpp"

namespace leakhunter::app {

/// Exit codes. Distinguishing "leaks found" from "tool failed" is what makes
/// LeakHunter usable as a CI gate.
enum class ExitCode : int {
    Success = 0,        ///< target ran and no leaks were found
    LeaksFound = 1,     ///< target ran, leaks were reported
    UsageError = 2,     ///< bad arguments
    LaunchError = 3,    ///< the target could not be started
    InternalError = 4,  ///< tracing or report generation failed
};

/// Runs one full session: launch -> trace -> analyse -> report.
///
/// The process runner is injected so tests can drive the pipeline without
/// spawning anything.
class Application {
public:
    Application(std::unique_ptr<process::IProcessRunner> runner, std::ostream& out,
                std::ostream& err);
    ~Application();

    [[nodiscard]] ExitCode run(const cli::Options& options);

    /// The report from the last run; empty before the first one.
    [[nodiscard]] const analysis::LeakReport* lastReport() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace leakhunter::app
