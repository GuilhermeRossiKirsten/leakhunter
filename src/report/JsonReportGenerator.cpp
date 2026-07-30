#include "leakhunter/report/JsonReportGenerator.hpp"

#include <fstream>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "leakhunter/core/Logger.hpp"

namespace leakhunter::report {
namespace {

using nlohmann::json;

[[nodiscard]] json frameToJson(const StackFrame& frame) {
    json node{
        {"address", fmt::format("0x{:x}", frame.address)},
        {"function", frame.function},
        {"displayName", frame.displayName()},
        {"module", frame.module},
        {"moduleOffset", fmt::format("0x{:x}", frame.moduleOffset())},
        {"symbolOffset", fmt::format("0x{:x}", frame.symbolOffset())},
        // False when the name came from dladdr's nearest exported symbol
        // rather than from DWARF, i.e. when it may not be the real function.
        {"preciseName", frame.preciseName()},
        {"resolved", frame.resolved},
    };
    if (!frame.file.empty()) {
        node["file"] = frame.file;
        node["line"] = frame.line;
        if (frame.column > 0) {
            node["column"] = frame.column;  // llvm-symbolizer only; absent with addr2line
        }
    }
    return node;
}

/// The blamed line and its context. Absent, not null, when there is no source:
/// a consumer checking `"snippet" in group` gets a straight answer.
[[nodiscard]] json snippetToJson(const SourceSnippet& snippet) {
    json node{
        {"file", snippet.file},
        {"firstLine", snippet.firstLine},
        {"blamedLine", snippet.blamedLine},
        {"lines", snippet.lines},
    };
    if (snippet.column > 0) {
        node["column"] = snippet.column;
    }
    return node;
}

[[nodiscard]] json traceToJson(const StackTrace& trace) {
    json frames = json::array();
    for (const StackFrame& frame : trace) {
        frames.push_back(frameToJson(frame));
    }
    return frames;
}

}  // namespace

JsonReportGenerator::JsonReportGenerator(bool prettyPrint) : prettyPrint_(prettyPrint) {}

json JsonReportGenerator::toJson(const analysis::LeakReport& report) {
    json document;

    // `schemaVersion` is the contract for anything consuming this file; bump it
    // on breaking changes. See docs/REPORT_FORMAT.md.
    //
    // v2 added `mismatchedFrees` and folded it into `summary.clean`. A v1
    // consumer that ignores unknown keys keeps working, but one that equates
    // `clean` with "no leaks" would now be wrong, so the version moved.
    document["schemaVersion"] = 2;
    document["tool"] = {
        {"name", "leakhunter"},
        {"version", report.toolVersion},
    };
    document["run"] = {
        {"command", report.targetCommand},
        {"pid", report.stats.pid},
        {"generatedAt", report.generatedAtIso8601},
        {"exitCode", report.process.exitCode},
        {"terminatingSignal", report.process.terminatingSignal},
        {"durationMs", report.process.durationMs},
    };

    document["summary"] = {
        {"totalAllocations", report.stats.totalAllocations},
        {"totalDeallocations", report.stats.totalDeallocations},
        {"totalBytesAllocated", report.stats.totalBytesAllocated},
        {"totalBytesFreed", report.stats.totalBytesFreed},
        {"leakedBytes", report.leakedBytes},
        {"leakCount", report.leakCount},
        {"leakGroups", report.groups.size()},
        {"runtimeLeakCount", report.runtimeLeakCount},
        {"runtimeLeakedBytes", report.runtimeLeakedBytes},
        {"peakLiveBytes", report.stats.peakLiveBytes},
        {"untrackedFrees", report.stats.untrackedFrees},
        {"droppedRecords", report.stats.droppedRecords},
        {"truncatedTraces", report.stats.truncatedTraces},
        {"suppressedLeaks", report.suppressedLeaks},
        {"suppressedBytes", report.suppressedBytes},
        // Excluded from leakCount/leakedBytes entirely, unlike the two above.
        {"suppressedByRules", report.suppressedByRules},
        {"suppressedByRulesBytes", report.suppressedByRulesBytes},
        {"unusedSuppressionRules", report.unusedRules.size()},
        {"mismatchedFreeCount", report.stats.mismatchedFrees},
        {"suppressedMismatches", report.suppressedMismatches},
        {"mismatchesSuppressedByRules", report.suppressedMismatchesByRules},
        // "active" | "suppressed" | "disabled". A zero count only means the
        // program is clean when this says "active".
        {"mismatchDetection", toString(report.mismatchCheck)},
        {"clean", report.clean()},
    };

    json groups = json::array();
    for (const analysis::LeakGroup& group : report.groups) {
        groups.push_back({
            {"function", group.function},
            {"module", group.module},
            {"location", group.location},
            {"totalBytes", group.totalBytes},
            {"count", group.count},
            {"threadCount", group.threadCount},
            {"blamedFrame", group.blamedFrame},
            {"stackTrace", traceToJson(group.representativeTrace)},
            {"leakIndices", group.leakIndices},
        });
        if (!group.snippet.empty()) {
            groups.back()["snippet"] = snippetToJson(group.snippet);
        }
    }
    document["groups"] = std::move(groups);

    json leaks = json::array();
    for (const analysis::Leak& leak : report.leaks) {
        const StackFrame* responsible = leak.responsible();
        leaks.push_back({
            {"address", fmt::format("0x{:x}", leak.address)},
            {"size", leak.size},
            {"kind", toString(leak.kind)},
            {"origin", leak.origin == analysis::LeakOrigin::Runtime ? "runtime" : "application"},
            {"threadId", leak.threadId},
            {"timestampNs", leak.timestampNs},
            {"responsibleFunction", responsible != nullptr ? responsible->displayName() : ""},
            {"responsibleFrame", leak.responsibleFrame},
            {"stackTrace", traceToJson(leak.trace)},
        });
    }
    document["leaks"] = std::move(leaks);

    json mismatches = json::array();
    for (const MismatchedFree& mismatch : report.mismatchedFrees) {
        const StackFrame* responsible = mismatch.responsible();
        mismatches.push_back({
            {"address", fmt::format("0x{:x}", mismatch.address)},
            {"size", mismatch.size},
            {"allocatedBy", toString(mismatch.allocatedBy)},
            {"releasedBy", toString(mismatch.releasedBy)},
            {"description", fmt::format("allocated with {}, released with {}",
                                        toSourceSpelling(mismatch.allocatedBy),
                                        toSourceSpelling(mismatch.releasedBy))},
            {"allocatedOnThread", mismatch.allocatedOnThread},
            {"releasedOnThread", mismatch.releasedOnThread},
            {"timestampNs", mismatch.timestampNs},
            {"responsibleFunction", responsible != nullptr ? responsible->displayName() : ""},
            {"responsibleFrame", mismatch.responsibleFrame},
            // The allocation stack, not the free stack -- see MismatchedFree.
            {"stackTrace", traceToJson(mismatch.trace)},
        });
        if (!mismatch.snippet.empty()) {
            mismatches.back()["snippet"] = snippetToJson(mismatch.snippet);
        }
    }
    document["mismatchedFrees"] = std::move(mismatches);

    // What the suppression rules actually did. Present even when empty so a
    // consumer can always tell "no rules" from "rules that hid nothing".
    json fired = json::array();
    for (const analysis::LeakReport::RuleHit& hit : report.ruleHits) {
        fired.push_back({
            {"rule", hit.rule},
            {"count", hit.count},
            {"bytes", hit.bytes},
        });
    }
    document["suppressions"] = {
        {"applied", std::move(fired)},
        {"unused", report.unusedRules},
    };

    return document;
}

std::string JsonReportGenerator::serialize(const json& document, bool prettyPrint) {
    // error_handler_t::replace substitutes U+FFFD for malformed UTF-8 instead
    // of throwing. A mangled name from an obfuscated binary, or a module path
    // that is simply not UTF-8 (perfectly legal on Linux), must not be able to
    // destroy the whole report.
    return document.dump(prettyPrint ? 2 : -1, ' ', false, json::error_handler_t::replace);
}

Status JsonReportGenerator::generate(const analysis::LeakReport& report,
                                     const std::filesystem::path& outputPath) {
    std::error_code ec;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            return Error{fmt::format("cannot create '{}': {}", outputPath.parent_path().string(),
                                     ec.message())};
        }
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Error{fmt::format("cannot write '{}'", outputPath.string())};
    }

    output << serialize(toJson(report), prettyPrint_) << '\n';
    if (!output) {
        return Error{fmt::format("failed while writing '{}'", outputPath.string())};
    }

    log::debug("wrote {}", outputPath.string());
    return {};
}

}  // namespace leakhunter::report
