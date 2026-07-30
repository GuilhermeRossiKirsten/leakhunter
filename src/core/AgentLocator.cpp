#include "leakhunter/core/AgentLocator.hpp"

#include <cstdlib>

#include <fmt/format.h>

#include "leakhunter/core/BuildConfig.hpp"

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace leakhunter::core {
namespace fs = std::filesystem;

fs::path AgentLocator::executablePath() {
#if defined(__linux__)
    std::error_code ec;
    const fs::path resolved = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : resolved;
#elif defined(__APPLE__)
    char buffer[4096];
    std::uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(fs::path{buffer}, ec);
    return ec ? fs::path{buffer} : resolved;
#else
    return {};
#endif
}

std::vector<fs::path> AgentLocator::candidates() {
    std::vector<fs::path> paths;

    if (const char* override = std::getenv("LEAKHUNTER_AGENT"); override != nullptr && *override) {
        paths.emplace_back(override);
    }

    if (const fs::path exe = executablePath(); !exe.empty()) {
        const fs::path dir = exe.parent_path();
        paths.push_back(dir / ".." / "lib" / build::kAgentLibraryName);
        paths.push_back(dir / ".." / "lib" / "leakhunter" / build::kAgentLibraryName);
        paths.push_back(dir / build::kAgentLibraryName);
    }

    paths.emplace_back(build::kAgentInstallPath);
    return paths;
}

Result<fs::path> AgentLocator::locate() {
    std::string tried;

    for (const fs::path& candidate : candidates()) {
        std::error_code ec;
        const fs::path normalized = fs::weakly_canonical(candidate, ec);
        const fs::path& probe = ec ? candidate : normalized;

        if (fs::exists(probe, ec) && fs::is_regular_file(probe, ec)) {
            return probe;
        }
        tried += fmt::format("\n    {}", probe.string());
    }

    return Error{fmt::format(
        "could not locate {} -- set LEAKHUNTER_AGENT to its full path.\n  searched:{}",
        build::kAgentLibraryName, tried)};
}

}  // namespace leakhunter::core
