/// @file JsonReportGenerator.hpp
/// @brief Machine-readable report (nlohmann/json).

#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

#include "leakhunter/report/IReportGenerator.hpp"

namespace leakhunter::report {

/// Stable, versioned JSON intended for CI gates and tooling.
/// See docs/REPORT_FORMAT.md for the schema.
class JsonReportGenerator final : public IReportGenerator {
public:
    explicit JsonReportGenerator(bool prettyPrint = true);

    [[nodiscard]] Status generate(const analysis::LeakReport& report,
                                  const std::filesystem::path& outputPath) override;

    [[nodiscard]] std::string_view defaultFileName() const noexcept override {
        return "report.json";
    }

    /// Serialisation without the file I/O -- reused by the HTML back-end, which
    /// embeds the same document, and by the tests.
    [[nodiscard]] static nlohmann::json toJson(const analysis::LeakReport& report);

    /// Renders @p document to text.
    ///
    /// Never throws on content: symbol names and module paths come from
    /// arbitrary binaries and from a filesystem where a path is just bytes, so
    /// neither is guaranteed to be valid UTF-8. nlohmann's default is to throw
    /// on the first bad byte, which would turn one odd symbol into a total
    /// failure to produce any report at all. Invalid sequences are replaced.
    [[nodiscard]] static std::string serialize(const nlohmann::json& document, bool prettyPrint);

private:
    bool prettyPrint_;
};

}  // namespace leakhunter::report
