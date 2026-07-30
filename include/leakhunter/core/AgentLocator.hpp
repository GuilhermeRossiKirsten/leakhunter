/// @file AgentLocator.hpp
/// @brief Finds libleakhunter_agent.so at runtime.

#pragma once

#include <filesystem>
#include <vector>

#include "leakhunter/core/Error.hpp"

namespace leakhunter::core {

/// The CLI has to inject a library it does not know the install prefix of: it
/// may run from a build tree, from /usr/local, or from a relocated package.
/// The search order is explicit and reported on failure so a broken install is
/// obvious instead of mysterious.
class AgentLocator {
public:
    /// Search order:
    ///   1. $LEAKHUNTER_AGENT
    ///   2. <exe-dir>/../lib/libleakhunter_agent.so        (build tree)
    ///   3. <exe-dir>/../lib/leakhunter/...                (installed layout)
    ///   4. <exe-dir>/libleakhunter_agent.so               (flat layout)
    ///   5. the compiled-in install path
    [[nodiscard]] static Result<std::filesystem::path> locate();

    /// Candidate paths, in order. Exposed for diagnostics and tests.
    [[nodiscard]] static std::vector<std::filesystem::path> candidates();

    /// Absolute path of the running executable, empty when unavailable.
    [[nodiscard]] static std::filesystem::path executablePath();
};

}  // namespace leakhunter::core
