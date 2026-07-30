/// @file IProcessRunner.hpp
/// @brief Abstraction over launching and waiting for the monitored program.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "leakhunter/core/Error.hpp"
#include "leakhunter/core/Types.hpp"

namespace leakhunter::process {

struct ProcessSpec {
    /// argv[0] plus arguments. Resolved through PATH when it has no separator.
    std::vector<std::string> command;

    /// Variables added to (or overriding) the inherited environment.
    std::map<std::string, std::string> environment;

    /// Library force-loaded into the child before any other object. Empty
    /// disables injection, which is what the unit tests use.
    std::filesystem::path preloadLibrary;

    std::filesystem::path workingDirectory;
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    /// Runs the program to completion. Returns an Error only when the process
    /// could not be started at all; a non-zero exit status is a valid result.
    [[nodiscard]] virtual Result<ProcessResult> run(const ProcessSpec& spec) = 0;
};

}  // namespace leakhunter::process
