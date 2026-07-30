/// @file Logger.hpp
/// @brief Thin facade over spdlog so the rest of the code never includes it.

#pragma once

#include <string_view>

#include <spdlog/spdlog.h>

namespace leakhunter::log {

enum class Level {
    Quiet,    ///< errors only
    Normal,   ///< errors + warnings + progress
    Verbose,  ///< everything, including per-stage timings
};

/// Installs the stderr sink. Safe to call more than once; the last call wins.
/// Reports themselves go to files, so stdout is left clean for the summary.
void initialize(Level level);

template <typename... Args>
void debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    spdlog::debug(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    spdlog::info(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    spdlog::warn(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    spdlog::error(fmt, std::forward<Args>(args)...);
}

}  // namespace leakhunter::log
