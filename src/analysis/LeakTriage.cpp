#include "leakhunter/analysis/LeakTriage.hpp"

#include <algorithm>

#include <fmt/format.h>

#include "leakhunter/analysis/LeakReport.hpp"

namespace leakhunter::analysis {
namespace {

constexpr double kNanosPerHour = 3600.0 * 1e9;

/// A site whose window covers more than this much of the run is growing with
/// uptime rather than with any one event.
constexpr double kSteadyFraction = 0.50;

/// ...and one that finished within this much of the start is a fixed cost.
constexpr double kStartupFraction = 0.10;

/// Below this, temporal classification means nothing.
///
/// A program that runs for ten milliseconds cannot tell "grows with uptime"
/// apart from "happened once" -- every site looks clustered because the whole
/// run is a cluster. Reporting a confident `burst` there would be inventing
/// information; saying the run was too short is the truth.
constexpr double kMinClassifiableNs = 250.0 * 1e6;

/// How long the run lasted, in nanoseconds.
///
/// `SessionStats::durationNs()` needs the end timestamp, which comes from the
/// trace's end record -- and a target *we* stopped never writes one. That is
/// exactly the long-running service case, which is where a growth rate matters
/// most, so falling back to the host's wall clock is not a detail.
[[nodiscard]] double runDurationNs(const LeakReport& report) {
    if (const std::uint64_t fromTrace = report.stats.durationNs(); fromTrace > 0) {
        return static_cast<double>(fromTrace);
    }
    return static_cast<double>(report.process.durationMs) * 1e6;
}

[[nodiscard]] std::string suppressionFor(const LeakGroup& group) {
    const StackFrame* blamed = group.blamedFrame < group.representativeTrace.size()
                                   ? &group.representativeTrace[group.blamedFrame]
                                   : nullptr;
    if (blamed == nullptr) {
        return {};
    }

    // Prefer the narrowest rule that will actually match. A `function:` rule
    // built from an approximate name (dladdr's nearest exported symbol) would
    // silence whatever that symbol really covers, which is not what the reader
    // asked for.
    if (blamed->preciseName() && !blamed->function.empty()) {
        // Drop the parameter list. A demangled C++ name carries its whole
        // signature, and pasting
        //   function:*poc::(anonymous namespace)::indexBatch(unsigned long)*
        // into a file is both unreadable and brittle -- it stops matching the
        // day someone adds a parameter. The qualified name alone is stable and
        // still specific.
        std::string name = blamed->function;
        if (name.back() == ')') {
            int depth = 0;
            for (std::size_t i = name.size(); i > 0; --i) {
                const char character = name[i - 1];
                if (character == ')') {
                    ++depth;
                } else if (character == '(') {
                    --depth;
                    if (depth == 0) {
                        name.resize(i - 1);
                        break;
                    }
                }
            }
        }
        return fmt::format("function:*{}*", name);
    }
    if (!blamed->file.empty()) {
        return fmt::format("file:*{}*", blamed->file);
    }
    if (!blamed->module.empty()) {
        return fmt::format("module:*{}*", blamed->module);
    }
    return {};
}

/// The release call that pairs with how the block was allocated.
[[nodiscard]] std::string_view ownerFor(AllocationKind kind) {
    switch (kind) {
        case AllocationKind::New:
            return "std::unique_ptr<T> (std::make_unique)";
        case AllocationKind::NewArray:
            return "std::vector<T>, or std::unique_ptr<T[]>";
        case AllocationKind::Malloc:
        case AllocationKind::Calloc:
        case AllocationKind::Realloc:
        case AllocationKind::AlignedAlloc:
            return "std::vector<std::byte>, or std::unique_ptr with a custom deleter";
        case AllocationKind::Unknown:
            break;
    }
    return "an owning type with a destructor";
}

/// The allocating entry point of a group, taken from its first listed leak.
[[nodiscard]] AllocationKind kindOf(const LeakReport& report, const LeakGroup& group) {
    for (const std::size_t index : group.leakIndices) {
        if (index < report.leaks.size()) {
            return report.leaks[index].kind;
        }
    }
    return AllocationKind::Unknown;
}

void buildAdvice(const LeakReport& report, LeakGroup& group) {
    LeakTriage& triage = group.triage;
    const AllocationKind kind = kindOf(report, group);

    switch (triage.pattern) {
        case LeakPattern::Steady:
            triage.advice.emplace_back(fmt::format(
                "Grows with uptime: about {}/hour at the rate observed here, {}/day. "
                "Fix this one first.",
                formatBytes(static_cast<std::uint64_t>(triage.bytesPerHour)),
                formatBytes(static_cast<std::uint64_t>(triage.bytesPerHour * 24.0))));
            break;

        case LeakPattern::Startup:
            triage.advice.emplace_back(
                "All of these were allocated early in the run, so the cost is fixed rather than "
                "growing. Often a deliberate one-time table or cache.");
            break;

        case LeakPattern::OneShot:
            triage.advice.emplace_back(
                "A single block. Frequently deliberate -- a singleton, a lazily built table.");
            break;

        case LeakPattern::Burst:
            triage.advice.emplace_back(
                "Clustered in time rather than spread across the run, so it is tied to some "
                "event. Whatever triggered it will trigger it again.");
            break;

        case LeakPattern::Unknown:
            break;
    }

    triage.advice.emplace_back(
        fmt::format("Allocated with {}. Holding it in {} makes the release automatic on every "
                    "path, including the ones that throw or return early.",
                    toSourceSpelling(kind), ownerFor(kind)));

    if (group.threadCount > 1) {
        triage.advice.emplace_back(fmt::format(
            "Reached from {} threads, so the fix has to hold under concurrency -- and any "
            "ownership you add here is shared, not thread-local.",
            group.threadCount));
    }

    if (triage.pattern == LeakPattern::Startup || triage.pattern == LeakPattern::OneShot) {
        triage.advice.emplace_back(
            "If it is deliberate, suppress it rather than ignore the report: a rule keeps the "
            "count honest and says so out loud, whereas ignoring teaches everyone to ignore.");
    }
}

}  // namespace

std::string_view toString(LeakPattern pattern) noexcept {
    switch (pattern) {
        case LeakPattern::OneShot: return "one-shot";
        case LeakPattern::Startup: return "startup";
        case LeakPattern::Burst: return "burst";
        case LeakPattern::Steady: return "steady";
        case LeakPattern::Unknown: break;
    }
    return "unknown";
}

std::string_view describe(LeakPattern pattern) noexcept {
    switch (pattern) {
        case LeakPattern::OneShot: return "a single block, often deliberate";
        case LeakPattern::Startup: return "allocated early; a fixed cost, not a growing one";
        case LeakPattern::Burst: return "clustered in time; tied to an event";
        case LeakPattern::Steady: return "spread across the run; grows with uptime";
        case LeakPattern::Unknown: break;
    }
    return "not enough timing information";
}

void triageLeaks(LeakReport& report) {
    const double duration = runDurationNs(report);

    for (LeakGroup& group : report.groups) {
        LeakTriage& triage = group.triage;

        // Timestamps come from the leaks that were listed individually, which
        // may be fewer than the site actually leaked.
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::size_t sampled = 0;

        for (const std::size_t index : group.leakIndices) {
            if (index >= report.leaks.size()) {
                continue;
            }
            const std::uint64_t when = report.leaks[index].timestampNs;
            if (sampled == 0) {
                first = when;
                last = when;
            } else {
                first = std::min(first, when);
                last = std::max(last, when);
            }
            ++sampled;
        }

        if (sampled == 0) {
            continue;  // nothing listed: leave the triage empty rather than guess
        }

        triage.firstSeenNs = first;
        triage.lastSeenNs = last;
        triage.sampleIsPartial = sampled < group.count;

        const double span = static_cast<double>(last - first);

        if (group.count == 1) {
            triage.pattern = LeakPattern::OneShot;
        } else if (duration < kMinClassifiableNs) {
            // Too short to tell anything apart. Leave the pattern unknown and
            // say why, rather than dress a guess up as a finding.
            triage.pattern = LeakPattern::Unknown;
            triage.advice.emplace_back(fmt::format(
                "The run lasted {:.0f} ms, which is too short to tell a leak that grows with "
                "uptime from one that happens once. Run the target for longer, or under a "
                "realistic workload, if you need that distinction.",
                duration / 1e6));
        } else if (duration <= 0.0) {
            // No usable duration -- a target that exited instantly, or a trace
            // with neither an end record nor a measured wall time. Say so
            // instead of dividing by it.
            triage.pattern = LeakPattern::Unknown;
        } else if (span >= duration * kSteadyFraction) {
            triage.pattern = LeakPattern::Steady;
            // Rate over the window actually observed, not over the whole run:
            // a site that started late must not be averaged down by the time
            // before it existed.
            if (span > 0.0) {
                triage.bytesPerHour = static_cast<double>(group.totalBytes) * kNanosPerHour / span;
            }
        } else if (static_cast<double>(last) <= duration * kStartupFraction) {
            triage.pattern = LeakPattern::Startup;
        } else {
            triage.pattern = LeakPattern::Burst;
        }

        triage.suppressionRule = suppressionFor(group);
        buildAdvice(report, group);
    }
}

}  // namespace leakhunter::analysis
