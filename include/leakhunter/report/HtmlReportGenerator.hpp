/// @file HtmlReportGenerator.hpp
/// @brief Self-contained single-file HTML report.

#pragma once

#include <string>

#include "leakhunter/report/IReportGenerator.hpp"

namespace leakhunter::report {

/// Emits one HTML file with inlined CSS and JS and the report data embedded as
/// JSON. No network access, no bundler, no assets directory: the file can be
/// attached to a ticket or published as a CI artifact and still work.
class HtmlReportGenerator final : public IReportGenerator {
public:
    [[nodiscard]] Status generate(const analysis::LeakReport& report,
                                  const std::filesystem::path& outputPath) override;

    [[nodiscard]] std::string_view defaultFileName() const noexcept override {
        return "report.html";
    }

    /// Renders to a string; used by the tests and by anyone embedding the
    /// report elsewhere.
    [[nodiscard]] static std::string render(const analysis::LeakReport& report);
};

}  // namespace leakhunter::report
