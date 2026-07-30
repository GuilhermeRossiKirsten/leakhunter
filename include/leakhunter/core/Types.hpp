/// @file Types.hpp
/// @brief Core value types shared by every host-side module.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "leakhunter/ipc/TraceFormat.hpp"

namespace leakhunter {

using AllocationKind = ipc::AllocKind;
using ReleaseKind = ipc::FreeKind;

[[nodiscard]] std::string_view toString(AllocationKind kind) noexcept;
[[nodiscard]] std::string_view toString(ReleaseKind kind) noexcept;

/// The C++ spelling of a kind, as it would appear in source: "new[]", "free".
/// Used in report prose, where "NewArray released by Free" reads worse than
/// "allocated with new[], released with free()".
[[nodiscard]] std::string_view toSourceSpelling(AllocationKind kind) noexcept;
[[nodiscard]] std::string_view toSourceSpelling(ReleaseKind kind) noexcept;

/// Whether releasing a block allocated with @p allocated via @p released is
/// defined behaviour.
///
/// The rules are the language's, not ours:
///   * malloc/calloc/realloc/aligned_alloc pair with free() and realloc();
///   * new pairs with delete, new[] pairs with delete[], and crossing those
///     two is undefined even when it happens to work;
///   * `Unknown` on either side means we could not observe one half of the
///     pair -- an older trace, or a block that was allocated before the agent
///     was live -- and is never reported as a mismatch.
///
/// realloc() on an aligned_alloc/posix_memalign block is deliberately treated
/// as legal: POSIX leaves it undefined, but glibc supports it and it is common
/// enough that flagging it would cost more in noise than it earns in bugs.
[[nodiscard]] bool isCompatibleRelease(AllocationKind allocated, ReleaseKind released) noexcept;

/// Human-readable byte count, e.g. "1.50 MiB".
[[nodiscard]] std::string formatBytes(std::uint64_t bytes);

/// One resolved entry of a call stack.
struct StackFrame {
    std::uint64_t address = 0;       ///< program counter (call site, see note below)
    std::uint64_t moduleBase = 0;    ///< load address of the owning object
    std::uint64_t symbolAddress = 0; ///< start of the containing symbol, 0 if unknown
    std::string function;            ///< demangled when possible, else mangled, else empty
    std::string module;              ///< path of the owning shared object / executable
    std::string file;                ///< source file, when debug info is available
    std::uint32_t line = 0;          ///< source line, 0 when unknown
    bool resolved = false;           ///< false => only a raw address is known

    /// Distance from the start of the named symbol.
    ///
    /// Worth showing when the name came from dladdr rather than DWARF: a
    /// stripped binary exposes only its dynamic symbols, so dladdr answers with
    /// the nearest *exported* symbol, which may be nowhere near the function
    /// that actually allocated. A large offset is the reader's signal that the
    /// name is an approximation.
    [[nodiscard]] std::uint64_t symbolOffset() const noexcept {
        return symbolAddress != 0 && address >= symbolAddress ? address - symbolAddress : 0;
    }

    /// True when the name is exact: DWARF gave us a file and line for it.
    [[nodiscard]] bool preciseName() const noexcept { return !file.empty() && line > 0; }

    /// Offset of the frame inside its module. This is what a symbolizer such as
    /// llvm-symbolizer expects for a position-independent executable.
    [[nodiscard]] std::uint64_t moduleOffset() const noexcept {
        return moduleBase != 0 && address >= moduleBase ? address - moduleBase : address;
    }

    /// Single-line rendering: "function at file:line", or
    /// "function+0x1f0 (module+0x...)" when the name is only approximate.
    [[nodiscard]] std::string describe() const;

    /// The name as a report should show it: bare when exact, suffixed with the
    /// offset when it is the nearest known symbol rather than the real one.
    [[nodiscard]] std::string displayName() const;
};

using StackTrace = std::vector<StackFrame>;

/// A single tracked allocation.
///
/// `callStack` holds the raw program counters captured by the agent; `trace`
/// is filled in later by the LeakAnalyzer via the SymbolResolver. Keeping both
/// means symbolisation stays an explicit, testable step rather than a hidden
/// side effect of reading the trace.
///
/// Addresses are `std::uint64_t` rather than `void*` on purpose: they belong to
/// a different process and must never be dereferenced by the host.
struct AllocationInfo {
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t timestampNs = 0;
    std::uint64_t threadId = 0;
    AllocationKind kind = AllocationKind::Unknown;
    std::vector<std::uint64_t> callStack;
    StackTrace trace;
};

/// A block released through the wrong entry point: `new[]` freed with
/// `delete`, `malloc` freed with `delete`, and so on.
///
/// This is undefined behaviour rather than a leak, and it is worth reporting
/// precisely because the program usually survives it -- right up until an
/// allocator change or a type gaining a destructor turns it into a crash.
///
/// Only the *allocation* stack is recorded. Capturing a stack on every free as
/// well would roughly double the cost of tracing to answer a question the
/// allocation site already answers in practice: you know the object, so you
/// know which delete is wrong. The free's thread id is kept because a release
/// on a different thread is a useful hint about ownership.
struct MismatchedFree {
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t timestampNs = 0;      ///< when the release happened
    std::uint64_t allocatedOnThread = 0;
    std::uint64_t releasedOnThread = 0;
    AllocationKind allocatedBy = AllocationKind::Unknown;
    ReleaseKind releasedBy = ReleaseKind::Unknown;

    std::vector<std::uint64_t> callStack;  ///< raw PCs of the allocation site
    StackTrace trace;                      ///< filled in by the analyser
    std::size_t responsibleFrame = 0;

    [[nodiscard]] const StackFrame* responsible() const noexcept {
        return responsibleFrame < trace.size() ? &trace[responsibleFrame] : nullptr;
    }
};

/// Aggregate counters for one monitored run.
struct SessionStats {
    std::uint64_t pid = 0;
    std::uint64_t totalAllocations = 0;
    std::uint64_t totalDeallocations = 0;
    std::uint64_t totalBytesAllocated = 0;
    std::uint64_t totalBytesFreed = 0;
    std::uint64_t untrackedFrees = 0;   ///< frees of pointers we never saw allocated
    std::uint64_t droppedRecords = 0;   ///< agent-side losses; non-zero => partial data
    std::uint64_t truncatedTraces = 0;

    /// Releases seen per entry point. Used to tell "this program never calls
    /// delete" from "our operator delete hook is not engaged" -- see
    /// AllocationRegistry::mismatchDetectionIsTrustworthy().
    std::uint64_t newAllocations = 0;
    std::uint64_t deleteReleases = 0;
    std::uint64_t mismatchedFrees = 0;  ///< total, including any not listed
    std::uint64_t peakLiveBytes = 0;
    std::uint64_t startTimestampNs = 0;
    std::uint64_t endTimestampNs = 0;

    [[nodiscard]] std::uint64_t durationNs() const noexcept {
        return endTimestampNs > startTimestampNs ? endTimestampNs - startTimestampNs : 0;
    }
};

/// Outcome of running the monitored program.
struct ProcessResult {
    int exitCode = 0;
    int terminatingSignal = 0;  ///< 0 unless the child died from a signal
    bool started = false;
    std::uint64_t pid = 0;
    std::uint64_t durationMs = 0;

    [[nodiscard]] bool exitedCleanly() const noexcept {
        return started && terminatingSignal == 0 && exitCode == 0;
    }
};

}  // namespace leakhunter
