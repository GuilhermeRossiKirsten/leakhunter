#include "leakhunter/analysis/LeakAnalyzer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "leakhunter/core/BuildConfig.hpp"
#include "leakhunter/core/Logger.hpp"

namespace leakhunter::analysis {
namespace {

/// Function names that are part of the allocation machinery rather than of the
/// code that requested the memory. A leak attributed to `operator new` is
/// useless; the first frame below these is the one a developer can act on.
constexpr std::array<std::string_view, 18> kAllocatorFunctions{
    "malloc",
    "calloc",
    "realloc",
    "aligned_alloc",
    "memalign",
    "posix_memalign",
    "valloc",
    "pvalloc",
    "strdup",
    "strndup",
    "operator new",
    "operator new[]",
    "operator delete",
    "operator delete[]",
    "_Znwm",
    "_Znam",
    "__libc_malloc",
    "__libc_calloc",
};

/// Our own agent: its frames sit on top of every captured stack and are never
/// the answer to "who allocated this".
constexpr std::string_view kAgentModule = "libleakhunter_agent";

/// The C runtime. A frame here is not automatically uninteresting -- if libc
/// allocated on the application's behalf we still want to walk out to the
/// caller -- but an allocation *requested* from here belongs to the runtime.
constexpr std::array<std::string_view, 5> kRuntimeModules{
    "libc.so", "libc-", "ld-linux", "ld.so", "libdl.so",
};

[[nodiscard]] bool moduleMatches(const std::string& module,
                                 const std::array<std::string_view, 5>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&module](std::string_view needle) {
        return module.find(needle) != std::string::npos;
    });
}

/// Matches an exact name or a name followed by its signature, so both
/// "operator new" and "operator new(unsigned long)" are recognised.
[[nodiscard]] bool matchesAllocatorName(std::string_view text) {
    return std::any_of(
        kAllocatorFunctions.begin(), kAllocatorFunctions.end(), [text](std::string_view needle) {
            if (text == needle) {
                return true;
            }
            return text.size() > needle.size() && text.starts_with(needle) &&
                   text[needle.size()] == '(';
        });
}

[[nodiscard]] std::string isoTimestampNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif

    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", utc.tm_year + 1900, utc.tm_mon + 1,
                       utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
}

/// Grouping key. Module is part of it so identically-named static functions in
/// different objects do not collapse into one bogus group.
[[nodiscard]] std::string groupKey(const StackFrame* frame, std::uint64_t fallbackAddress) {
    if (frame == nullptr || (frame->function.empty() && frame->module.empty())) {
        return fmt::format("<unknown>@0x{:x}", fallbackAddress);
    }
    if (frame->function.empty()) {
        return fmt::format("{}+0x{:x}", frame->module, frame->moduleOffset());
    }
    return fmt::format("{}|{}", frame->function, frame->module);
}

}  // namespace

std::string_view toString(MismatchCheck state) noexcept {
    switch (state) {
        case MismatchCheck::Active: return "active";
        case MismatchCheck::Suppressed: return "suppressed";
        case MismatchCheck::Disabled: return "disabled";
    }
    return "unknown";
}

LeakAnalyzer::LeakAnalyzer(const symbols::ISymbolResolver& resolver, AnalyzerConfig config)
    : resolver_(resolver), config_(config) {}

bool LeakAnalyzer::isAllocatorFrame(const StackFrame& frame) {
    if (frame.module.find(kAgentModule) != std::string::npos) {
        return true;
    }
    if (frame.function.empty()) {
        return false;  // unnamed but outside the agent: keep it as a candidate
    }
    return matchesAllocatorName(frame.function);
}

LeakOrigin LeakAnalyzer::classifyOrigin(const StackTrace& trace) {
    for (const StackFrame& frame : trace) {
        if (isAllocatorFrame(frame)) {
            continue;
        }
        // First frame that actually asked for memory. If it lives in the C
        // runtime, the runtime owns this block -- typically a stdio buffer or
        // a locale table that is never released by design.
        return moduleMatches(frame.module, kRuntimeModules) ? LeakOrigin::Runtime
                                                            : LeakOrigin::Application;
    }
    return LeakOrigin::Application;
}

std::size_t LeakAnalyzer::findResponsibleFrame(const StackTrace& trace) {
    // Skip the allocator machinery, then keep walking out of the C runtime:
    // "leaked in memcpy" is never a useful answer, "leaked in loadConfig" is.
    for (std::size_t i = 0; i < trace.size(); ++i) {
        if (!isAllocatorFrame(trace[i]) && !moduleMatches(trace[i].module, kRuntimeModules)) {
            return i;
        }
    }
    for (std::size_t i = 0; i < trace.size(); ++i) {
        if (!isAllocatorFrame(trace[i])) {
            return i;  // entirely inside the runtime; blame its innermost frame
        }
    }
    return 0;
}

LeakReport LeakAnalyzer::analyze(std::vector<AllocationInfo> liveAllocations,
                                 std::vector<MismatchedFree> mismatches,
                                 const SessionStats& stats) {
    LeakReport report;
    report.stats = stats;
    report.toolVersion = build::kVersion;
    report.generatedAtIso8601 = isoTimestampNow();

    // Biggest first: that is the order in which a developer wants to fix them.
    std::sort(liveAllocations.begin(), liveAllocations.end(),
              [](const AllocationInfo& lhs, const AllocationInfo& rhs) {
                  if (lhs.size != rhs.size) {
                      return lhs.size > rhs.size;
                  }
                  return lhs.timestampNs < rhs.timestampNs;
              });

    report.leaks.reserve(std::min(liveAllocations.size(), config_.maxDetailedLeaks));

    // Bytes attributed to each suppression rule, by rule index.
    std::unordered_map<std::size_t, std::uint64_t> ruleBytes;

    for (AllocationInfo& allocation : liveAllocations) {
        Leak leak;
        leak.address = allocation.address;
        leak.size = allocation.size;
        leak.threadId = allocation.threadId;
        leak.kind = allocation.kind;
        leak.timestampNs = allocation.timestampNs > stats.startTimestampNs
                               ? allocation.timestampNs - stats.startTimestampNs
                               : allocation.timestampNs;
        leak.trace = resolver_.resolveAll(allocation.callStack);
        leak.responsibleFrame = findResponsibleFrame(leak.trace);
        leak.origin = classifyOrigin(leak.trace);

        // Suppression comes first: a rule the user wrote outranks our own
        // runtime/application classification, and the leak must not reach the
        // headline counters at all.
        if (config_.suppressions != nullptr) {
            const std::size_t rule =
                config_.suppressions->match(leak.trace, leak.responsibleFrame);
            if (rule != SuppressionSet::npos) {
                config_.suppressions->noteHit(rule);
                ++report.suppressedByRules;
                report.suppressedByRulesBytes += leak.size;
                ruleBytes[rule] += leak.size;
                continue;
            }
        }

        if (leak.origin == LeakOrigin::Runtime) {
            ++report.runtimeLeakCount;
            report.runtimeLeakedBytes += leak.size;
            if (!config_.includeRuntimeLeaks) {
                continue;  // counted in its own bucket, kept out of the listing
            }
        }

        report.leakedBytes += leak.size;
        ++report.leakCount;

        const bool tooSmall = leak.size < config_.minLeakSize;
        const bool tooMany = report.leaks.size() >= config_.maxDetailedLeaks;
        if (tooSmall || tooMany) {
            ++report.suppressedLeaks;
            report.suppressedBytes += leak.size;
            continue;
        }

        report.leaks.push_back(std::move(leak));
    }

    buildGroups(report);
    // addMismatches may credit further hits to the rules, so the summary has to
    // come after it -- it reads the hit counts back off the SuppressionSet.
    addMismatches(report, std::move(mismatches));
    summariseSuppressions(report, ruleBytes);
    // Triage deliberately does NOT run here. It needs the run's duration, and
    // for a target we stopped that only exists in ProcessResult, which the
    // Application fills in after this returns.

    log::debug("analysed {} leaks ({} bytes) into {} groups", report.leakCount, report.leakedBytes,
               report.groups.size());
    return report;
}

void LeakAnalyzer::summariseSuppressions(
    LeakReport& report, const std::unordered_map<std::size_t, std::uint64_t>& ruleBytes) const {
    if (config_.suppressions == nullptr) {
        return;
    }

    const std::vector<SuppressionRule>& rules = config_.suppressions->rules();
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (rules[i].hits == 0) {
            report.unusedRules.push_back(rules[i].describe());
            continue;
        }
        const auto bytes = ruleBytes.find(i);
        report.ruleHits.push_back({rules[i].describe(), rules[i].hits,
                                   bytes != ruleBytes.end() ? bytes->second : 0});
    }

    // Biggest suppression first: if one rule is hiding most of the leaked
    // memory, that is the one worth re-examining.
    std::sort(report.ruleHits.begin(), report.ruleHits.end(),
              [](const LeakReport::RuleHit& lhs, const LeakReport::RuleHit& rhs) {
                  return lhs.bytes > rhs.bytes;
              });

    if (report.suppressedByRules > 0) {
        log::info("{} leak(s) totalling {} were suppressed by {} rule(s)", report.suppressedByRules,
                  formatBytes(report.suppressedByRulesBytes), report.ruleHits.size());
    }
    for (const std::string& unused : report.unusedRules) {
        // A rule that never fires is not coverage, and looks like it is.
        log::warn("suppression rule matched nothing: {}", unused);
    }
}

void LeakAnalyzer::addMismatches(LeakReport& report,
                                 std::vector<MismatchedFree> mismatches) const {
    if (!config_.detectMismatchedFrees) {
        // Zeroing the counter too, because it is what report.clean() reads: an
        // opt-out has to opt out of the exit code as well, not just the listing.
        report.stats.mismatchedFrees = 0;
        return;
    }

    const std::size_t listed = std::min(mismatches.size(), config_.maxDetailedMismatches);

    report.mismatchedFrees.reserve(listed);
    for (std::size_t i = 0; i < listed; ++i) {
        MismatchedFree& mismatch = mismatches[i];
        mismatch.trace = resolver_.resolveAll(mismatch.callStack);
        mismatch.responsibleFrame = findResponsibleFrame(mismatch.trace);
        mismatch.timestampNs = mismatch.timestampNs > report.stats.startTimestampNs
                                   ? mismatch.timestampNs - report.stats.startTimestampNs
                                   : mismatch.timestampNs;

        // Suppression rules cover mismatched frees too. Not doing so would be a
        // trap: a user with vendored code they cannot fix would write rules,
        // still get exit 1, and have nothing in the output explaining why.
        if (config_.suppressions != nullptr) {
            const std::size_t rule =
                config_.suppressions->match(mismatch.trace, mismatch.responsibleFrame);
            if (rule != SuppressionSet::npos) {
                config_.suppressions->noteHit(rule);
                // Deliberately not adding to ruleBytes. A mismatched block was
                // returned -- it is undefined behaviour, not lost memory -- so
                // folding its size into a "bytes suppressed" figure would
                // overstate the memory impact. It counts, it does not weigh.
                ++report.suppressedMismatchesByRules;
                continue;
            }
        }

        report.mismatchedFrees.push_back(std::move(mismatch));
    }

    // Only what survived suppression counts against the run. The incoming
    // stats.mismatchedFrees is the registry's true total, which may exceed what
    // it recorded individually; whatever is left over after the survivors is
    // "counted but not listed".
    const auto surviving = static_cast<std::uint64_t>(report.mismatchedFrees.size());
    report.stats.mismatchedFrees -=
        std::min(report.stats.mismatchedFrees, report.suppressedMismatchesByRules);
    report.suppressedMismatches =
        report.stats.mismatchedFrees > surviving ? report.stats.mismatchedFrees - surviving : 0;

    if (report.stats.mismatchedFrees > 0) {
        log::warn("{} block(s) were released through the wrong entry point -- this is undefined "
                  "behaviour, see the report",
                  report.stats.mismatchedFrees);
    }
}

void LeakAnalyzer::rankHotSpots(LeakReport& report, std::vector<AllocationSite> sites,
                                std::size_t keep) const {
    if (sites.empty() || keep == 0) {
        return;
    }

    // The registry keys sites by the whole call stack, because it has no
    // symbols to key on anything better. That splits one function called from
    // three places into three rows with the same name and no way to tell them
    // apart, so merge them here on the same key the leak groups use: developers
    // think "makeRawBuffer allocated 1.1 MiB", not "makeRawBuffer as reached
    // from A allocated 500 KiB".
    //
    // Every site is symbolised to do that, which is cheap: the DWARF pass
    // already ran over these program counters during trace replay, so
    // resolveAll() is hash lookups against a table that is already built.
    std::unordered_map<std::string, std::size_t> keyToIndex;

    for (AllocationSite& site : sites) {
        StackTrace trace = resolver_.resolveAll(site.callStack);
        const std::size_t blamed = findResponsibleFrame(trace);

        // Computed before `trace` is moved into the spot below. Origin is a
        // property of the stack, so one registry site -- one exact stack -- has
        // exactly one origin.
        const LeakOrigin origin = classifyOrigin(trace);

        const StackFrame* frame = blamed < trace.size() ? &trace[blamed] : nullptr;

        const std::string key =
            groupKey(frame, site.callStack.empty() ? 0 : site.callStack.front());

        auto [position, inserted] = keyToIndex.try_emplace(key, report.hotSpots.size());
        if (inserted) {
            HotSpot spot;
            if (frame != nullptr) {
                spot.function = frame->displayName();
                spot.module = frame->module;
                if (frame->preciseName()) {
                    spot.location = fmt::format("{}:{}", frame->file, frame->line);
                } else if (!frame->module.empty()) {
                    spot.location =
                        fmt::format("{}+0x{:x}", frame->module, frame->moduleOffset());
                }
            } else {
                spot.function = "<unknown>";
            }
            spot.representativeTrace = std::move(trace);
            spot.blamedFrame = blamed;
            report.hotSpots.push_back(std::move(spot));
        }

        HotSpot& spot = report.hotSpots[position->second];
        spot.totalBytes += site.totalBytes;
        spot.count += site.count;

        // A merged spot can hold both kinds: `main` can have allocations of its
        // own *and* be the frame blamed for a stdio buffer, which is exactly
        // what made a PASSED run display bytes in red.
        if (origin == LeakOrigin::Runtime) {
            spot.runtimeLiveBytes += site.liveBytes;
            spot.runtimeLiveCount += site.liveCount;
        } else {
            spot.liveBytes += site.liveBytes;
            spot.liveCount += site.liveCount;
        }

        // Summed, not maxed. Two call paths into one function can hold their
        // blocks at the same time, so the site's true high-water mark is at
        // most the sum -- and taking the max would report less memory than the
        // site demonstrably held.
        spot.peakLiveBytes += site.peakLiveBytes;
    }

    std::sort(report.hotSpots.begin(), report.hotSpots.end(),
              [](const HotSpot& lhs, const HotSpot& rhs) {
                  if (lhs.totalBytes != rhs.totalBytes) {
                      return lhs.totalBytes > rhs.totalBytes;
                  }
                  // Ties broken deterministically, so two runs of the same
                  // program produce the same report.
                  if (lhs.count != rhs.count) {
                      return lhs.count > rhs.count;
                  }
                  return lhs.function < rhs.function;
              });

    if (report.hotSpots.size() > keep) {
        report.hotSpots.resize(keep);
    }
}

void LeakAnalyzer::buildGroups(LeakReport& report) const {
    std::unordered_map<std::string, std::size_t> keyToIndex;
    std::vector<std::unordered_set<std::uint64_t>> threadsPerGroup;

    for (std::size_t leakIndex = 0; leakIndex < report.leaks.size(); ++leakIndex) {
        const Leak& leak = report.leaks[leakIndex];
        const StackFrame* frame = leak.responsible();
        const std::string key = groupKey(frame, leak.address);

        auto [position, inserted] = keyToIndex.try_emplace(key, report.groups.size());
        if (inserted) {
            LeakGroup group;
            // displayName() carries the offset when the name is only the
            // nearest exported symbol, so the report never presents a guess as
            // a fact.
            group.function = frame != nullptr ? frame->displayName() : "<unknown>";
            group.module = frame != nullptr ? frame->module : std::string{};

            if (frame != nullptr && frame->preciseName()) {
                group.location = fmt::format("{}:{}", frame->file, frame->line);
            } else if (frame != nullptr && !frame->module.empty()) {
                // No debug info, so the name is whatever symbol table lookup
                // came closest -- in a stripped binary that can be an exported
                // function some distance away. Show the module offset instead
                // of just the module: it is exact, and it is what a symbolizer
                // or a disassembler needs to find the real call site.
                group.location = fmt::format("{}+0x{:x}", frame->module, frame->moduleOffset());
            }
            group.representativeTrace = leak.trace;
            group.blamedFrame = leak.responsibleFrame;
            report.groups.push_back(std::move(group));
            threadsPerGroup.emplace_back();
        }

        LeakGroup& group = report.groups[position->second];
        group.totalBytes += leak.size;
        ++group.count;
        group.leakIndices.push_back(leakIndex);
        threadsPerGroup[position->second].insert(leak.threadId);
    }

    for (std::size_t i = 0; i < report.groups.size(); ++i) {
        report.groups[i].threadCount = threadsPerGroup[i].size();
    }

    // Safe to sort in place: keyToIndex is dead by now, and `leakIndices`
    // points into report.leaks, which is not being reordered.
    std::sort(report.groups.begin(), report.groups.end(),
              [](const LeakGroup& lhs, const LeakGroup& rhs) {
                  if (lhs.totalBytes != rhs.totalBytes) {
                      return lhs.totalBytes > rhs.totalBytes;
                  }
                  return lhs.count > rhs.count;
              });
}

}  // namespace leakhunter::analysis
