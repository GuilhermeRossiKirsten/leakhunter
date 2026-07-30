/// @file IReportGenerator.hpp
/// @brief Rendering contract for report back-ends.

#pragma once

#include <filesystem>
#include <string_view>

#include "leakhunter/analysis/LeakReport.hpp"
#include "leakhunter/core/Error.hpp"

namespace leakhunter::report {

class IReportGenerator {
public:
    virtual ~IReportGenerator() = default;

    /// Writes the report to @p outputPath, creating parent directories.
    [[nodiscard]] virtual Status generate(const analysis::LeakReport& report,
                                          const std::filesystem::path& outputPath) = 0;

    /// Default file name for this back-end, e.g. "report.html".
    [[nodiscard]] virtual std::string_view defaultFileName() const noexcept = 0;
};

}  // namespace leakhunter::report
