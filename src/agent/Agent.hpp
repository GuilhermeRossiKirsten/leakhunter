/// @file Agent.hpp
/// @brief In-process tracing runtime, loaded into the target via LD_PRELOAD.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "PcRegistry.hpp"
#include "TraceWriter.hpp"
#include "leakhunter/ipc/TraceFormat.hpp"

namespace leakhunter::agent {

/// Owns the trace file, the counters, and the reentrancy policy.
///
/// Deliberate deviations from the rest of the codebase, all forced by running
/// underneath the allocator:
///   * no constructors or destructors -- the object lives in BSS and is
///     initialised explicitly from the library constructor, because dynamic
///     initialisation order relative to libc is not something we control;
///   * no STL containers, no exceptions, no iostreams on any hot path;
///   * failures degrade (records get dropped and counted) instead of throwing.
///
/// Everything the agent knows is shipped to the host process, which is where
/// the actual analysis happens with normal C++.
class Agent {
public:
    /// The single instance. Not a Meyers singleton: a function-local static
    /// would need a guard variable and could run after first use.
    [[nodiscard]] static Agent& instance() noexcept;

    /// Reads the environment, opens the trace and starts recording.
    /// Called from the library constructor.
    void initialize() noexcept;

    /// Emits the symbol table and the end record, then closes the trace.
    /// Called from the library destructor.
    void shutdown() noexcept;

    [[nodiscard]] bool active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    void recordAllocation(void* address, std::size_t size, ipc::AllocKind kind) noexcept;
    void recordDeallocation(void* address, ipc::FreeKind kind) noexcept;

    /// Installs pthread_atfork handlers. See the note in Agent.cpp about why
    /// tracing stops in forked children.
    void installForkHandlers() noexcept;

    /// Flushes whatever is buffered, without the end marker.
    ///
    /// The buffered records of a program that dies abruptly are worth more than
    /// they cost, and a crashing program is exactly when someone reaches for a
    /// leak detector. Called from the fatal-signal handler and from the
    /// interposed `_exit`, neither of which runs library destructors.
    void emergencyFlush() noexcept;

private:
    void writeFileHeader() noexcept;
    void writeModuleMap() noexcept;
    void writeSymbolTable() noexcept;
    void writeEndRecord() noexcept;
    void installCrashHandlers() noexcept;

    std::atomic<bool> active_{false};
    std::atomic<bool> initialized_{false};

    std::atomic<std::uint64_t> totalAllocations_{0};
    std::atomic<std::uint64_t> totalDeallocations_{0};
    std::atomic<std::uint64_t> totalBytesAllocated_{0};
    std::atomic<std::uint64_t> truncatedTraces_{0};

    std::uint16_t maxFrames_ = 32;
    bool verbose_ = false;
    std::uint64_t startTimestampNs_ = 0;

    TraceWriter writer_;
    PcRegistry programCounters_;
};

/// RAII reentrancy guard.
///
/// Any work the agent does -- unwinding, writing, resolving symbols -- can call
/// back into malloc. `engaged()` is true only for the outermost entry on this
/// thread, and only that one records anything.
class HookGuard {
public:
    HookGuard() noexcept;
    ~HookGuard() noexcept;

    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;

    [[nodiscard]] bool engaged() const noexcept { return engaged_; }

private:
    bool engaged_;
};

/// Monotonic nanoseconds since an arbitrary origin. vDSO-backed, no syscall.
[[nodiscard]] std::uint64_t monotonicNanos() noexcept;

/// Kernel thread id, cached per thread.
[[nodiscard]] std::uint64_t currentThreadId() noexcept;

}  // namespace leakhunter::agent
