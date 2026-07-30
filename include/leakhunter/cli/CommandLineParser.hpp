/// @file CommandLineParser.hpp
/// @brief Hand-written argv parser (no third-party CLI dependency).

#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "leakhunter/cli/Options.hpp"
#include "leakhunter/core/Error.hpp"

namespace leakhunter::cli {

/// Grammar:
///   leakhunter [options] <program> [program-args...]
///   leakhunter [options] -- <program> [program-args...]
///
/// The first non-option token ends LeakHunter's own options: everything after
/// it belongs to the monitored program, so `leakhunter ./app --verbose` passes
/// `--verbose` to `./app`, not to LeakHunter. `--` forces the split explicitly.
class CommandLineParser {
public:
    /// @param out where `--help` and `--version` are written. Errors are not:
    ///        they come back as an Error for the caller to place, which is why
    ///        there is no error stream here.
    explicit CommandLineParser(std::ostream& out);

    [[nodiscard]] Result<Options> parse(std::span<const std::string_view> args) const;

    /// Convenience overload for main().
    [[nodiscard]] Result<Options> parse(int argc, char** argv) const;

    void printUsage() const;
    void printVersion() const;

private:
    std::ostream& out_;
};

}  // namespace leakhunter::cli
