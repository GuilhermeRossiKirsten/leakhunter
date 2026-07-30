#include "leakhunter/core/Logger.hpp"

#include <memory>

// The stderr colour sinks live in the stdout header in spdlog.
#include <spdlog/sinks/stdout_color_sinks.h>

namespace leakhunter::log {
namespace {

spdlog::level::level_enum toSpdlogLevel(Level level) {
    switch (level) {
        case Level::Quiet: return spdlog::level::err;
        case Level::Verbose: return spdlog::level::debug;
        case Level::Normal: break;
    }
    return spdlog::level::info;
}

}  // namespace

void initialize(Level level) {
    // Diagnostics go to stderr so the summary on stdout stays pipeable.
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("leakhunter", std::move(sink));

    logger->set_pattern("%^[leakhunter %l]%$ %v");
    logger->set_level(toSpdlogLevel(level));
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(toSpdlogLevel(level));
}

}  // namespace leakhunter::log
