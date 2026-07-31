#include "leakhunter/analysis/Baseline.hpp"

#include <algorithm>
#include <fstream>
#include <unordered_map>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "leakhunter/analysis/LeakReport.hpp"

namespace leakhunter::analysis {
namespace {

/// Identity of a site across runs.
///
/// Function plus module, matching how leak groups are keyed. Deliberately not
/// the source line: adding a line above a leak would otherwise present it as a
/// brand new finding and fail a build that changed nothing.
[[nodiscard]] std::string siteKey(const std::string& function, const std::string& module) {
    return fmt::format("{}|{}", function, module);
}

struct BaselineSite {
    std::uint64_t bytes = 0;
    std::uint64_t count = 0;
    std::string location;
};

}  // namespace

Result<BaselineDiff> compareToBaseline(const LeakReport& report,
                                       const std::string& baselinePath,
                                       double tolerancePercent) {
    std::ifstream file(baselinePath);
    if (!file) {
        return Error{fmt::format("cannot open baseline '{}'", baselinePath)};
    }

    nlohmann::json baseline;
    try {
        file >> baseline;
    } catch (const nlohmann::json::exception& error) {
        return Error{fmt::format("baseline '{}' is not valid JSON: {}", baselinePath, error.what())};
    }

    if (!baseline.contains("groups") || !baseline["groups"].is_array()) {
        return Error{fmt::format("baseline '{}' has no 'groups' array -- is it a LeakHunter report?",
                                 baselinePath)};
    }

    BaselineDiff diff;
    diff.loaded = true;
    diff.baselinePath = baselinePath;
    if (baseline.contains("run") && baseline["run"].contains("generatedAt")) {
        diff.baselineGeneratedAt = baseline["run"]["generatedAt"].get<std::string>();
    }
    if (baseline.contains("summary") && baseline["summary"].contains("mismatchedFreeCount")) {
        diff.baselineMismatches = baseline["summary"]["mismatchedFreeCount"].get<std::uint64_t>();
    }
    diff.currentMismatches = report.stats.mismatchedFrees;
    diff.tolerancePercent = std::max(0.0, tolerancePercent);

    std::unordered_map<std::string, BaselineSite> previous;
    for (const auto& group : baseline["groups"]) {
        BaselineSite site;
        site.bytes = group.value("totalBytes", std::uint64_t{0});
        site.count = group.value("count", std::uint64_t{0});
        site.location = group.value("location", std::string{});
        previous.emplace(siteKey(group.value("function", std::string{}),
                                 group.value("module", std::string{})),
                         std::move(site));
    }

    for (const LeakGroup& group : report.groups) {
        const std::string key = siteKey(group.function, group.module);

        SiteDelta delta;
        delta.function = group.function;
        delta.location = group.location;
        delta.currentBytes = group.totalBytes;
        delta.currentCount = group.count;

        const auto found = previous.find(key);
        if (found != previous.end()) {
            delta.baselineBytes = found->second.bytes;
            delta.baselineCount = found->second.count;
            previous.erase(found);  // what remains was fixed
        }

        if (delta.isNew()) {
            // A site the baseline never had is new whatever the tolerance says.
            // Tolerance is for noise in a known quantity, not for admitting
            // findings that did not exist before.
            diff.newSites.push_back(std::move(delta));
        } else if (delta.isWorse()) {
            const double allowed = static_cast<double>(delta.baselineCount) *
                                   (1.0 + diff.tolerancePercent / 100.0);
            if (static_cast<double>(delta.currentCount) > allowed) {
                diff.worseSites.push_back(std::move(delta));
            } else {
                diff.withinTolerance.push_back(std::move(delta));
            }
        } else {
            ++diff.unchangedSites;
        }
    }

    // Whatever the baseline had and this run does not: fixed. Reporting these
    // is not decoration -- a gate that only ever says "you broke something"
    // gets read as noise, and the fixed count is what shows the line moving.
    for (auto& [key, site] : previous) {
        SiteDelta delta;
        delta.baselineBytes = site.bytes;
        delta.baselineCount = site.count;
        delta.location = site.location;
        const std::string::size_type bar = key.find('|');
        delta.function = bar == std::string::npos ? key : key.substr(0, bar);
        diff.fixedSites.push_back(std::move(delta));
    }

    const auto byBytes = [](const SiteDelta& lhs, const SiteDelta& rhs) {
        if (lhs.currentBytes != rhs.currentBytes) {
            return lhs.currentBytes > rhs.currentBytes;
        }
        return lhs.function < rhs.function;  // deterministic across runs
    };
    std::sort(diff.newSites.begin(), diff.newSites.end(), byBytes);
    std::sort(diff.worseSites.begin(), diff.worseSites.end(), byBytes);
    std::sort(diff.fixedSites.begin(), diff.fixedSites.end(),
              [](const SiteDelta& lhs, const SiteDelta& rhs) {
                  if (lhs.baselineBytes != rhs.baselineBytes) {
                      return lhs.baselineBytes > rhs.baselineBytes;
                  }
                  return lhs.function < rhs.function;
              });

    return diff;
}

}  // namespace leakhunter::analysis
