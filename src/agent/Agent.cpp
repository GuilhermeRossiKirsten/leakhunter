#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "Agent.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "RealFunctions.hpp"
#include "StackTraceCollector.hpp"

namespace leakhunter::agent {
namespace {

/// Frames belonging to the interception machinery itself. Dropping them here
/// keeps them out of the trace entirely instead of filtering them later.
constexpr std::size_t kSkipFrames = 2;

/// The agent lives in BSS: constant-initialised before any code runs, so it is
/// usable from the very first interposed call, and trivially destructible so
/// nothing tears it down behind our back during exit. `constinit` makes that a
/// compile-time guarantee rather than an assumption.
constinit Agent g_agent;

__attribute__((tls_model("initial-exec"))) thread_local int t_hookDepth = 0;
__attribute__((tls_model("initial-exec"))) thread_local std::uint64_t t_threadId = 0;

/// Writes straight to fd 2. fprintf is off limits here: it allocates.
void diagnostic(const char* message) noexcept {
    const std::size_t length = std::strlen(message);
    [[maybe_unused]] const ssize_t ignored = ::write(STDERR_FILENO, message, length);
}

}  // namespace

std::uint64_t monotonicNanos() noexcept {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

std::uint64_t currentThreadId() noexcept {
    if (t_threadId == 0) {
        t_threadId = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    }
    return t_threadId;
}

HookGuard::HookGuard() noexcept : engaged_(t_hookDepth == 0) { ++t_hookDepth; }
HookGuard::~HookGuard() noexcept { --t_hookDepth; }

Agent& Agent::instance() noexcept { return g_agent; }

void Agent::writeFileHeader() noexcept {
    ipc::FileHeader header{};
    header.magic = ipc::kMagic;
    header.version = ipc::kFormatVersion;
    header.pid = static_cast<std::uint64_t>(::getpid());
    header.startTimestampNs = startTimestampNs_;
    header.pointerSize = sizeof(void*);
    header.maxFrames = maxFrames_;

    const TraceWriter::Chunk chunks[] = {{&header, sizeof(header)}};
    writer_.appendRecord(chunks, 1);
}

namespace {

/// Absolute path of the running executable, resolved once.
///
/// dl_iterate_phdr reports the main executable with an empty name, and the
/// obvious substitute -- "/proc/self/exe" -- is a trap: the host reads this
/// trace in a *different* process, where that path points at leakhunter
/// itself. It has to be resolved here, while "self" still means the target.
const char* mainExecutablePath() noexcept {
    static char resolved[4096];
    static bool ready = false;

    if (!ready) {
        ready = true;
        const ssize_t length = ::readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
        resolved[length > 0 ? length : 0] = '\0';
    }
    return resolved;
}

/// dl_iterate_phdr callback: one ModuleRecord per loaded object.
int emitModule(struct dl_phdr_info* info, std::size_t /*infoSize*/, void* argument) {
    auto* writer = static_cast<TraceWriter*>(argument);

    const char* path = info->dlpi_name != nullptr && info->dlpi_name[0] != '\0'
                           ? info->dlpi_name
                           : mainExecutablePath();
    if (path[0] == '\0') {
        return 0;  // unnameable object; its addresses stay unattributed
    }

    // Span = end of the highest loadable segment. Used only to attribute an
    // address to an object, so an over-estimate is harmless and a miss is not.
    std::uint64_t span = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& header = info->dlpi_phdr[i];
        if (header.p_type == PT_LOAD) {
            const std::uint64_t end = header.p_vaddr + header.p_memsz;
            span = end > span ? end : span;
        }
    }

    const std::size_t pathBytes = std::min<std::size_t>(std::strlen(path), 0xFFFFU);

    ipc::ModuleRecord record{};
    record.base = static_cast<std::uint64_t>(info->dlpi_addr);
    record.span = span;
    record.pathBytes = static_cast<std::uint16_t>(pathBytes);

    ipc::RecordHeader header{};
    header.type = static_cast<std::uint8_t>(ipc::RecordType::Module);
    header.payloadBytes = static_cast<std::uint32_t>(sizeof(record) + pathBytes);

    const TraceWriter::Chunk chunks[] = {
        {&header, sizeof(header)},
        {&record, sizeof(record)},
        {path, pathBytes},
    };
    writer->appendRecord(chunks, 3);
    return 0;
}

}  // namespace

void Agent::writeModuleMap() noexcept {
    ::dl_iterate_phdr(&emitModule, &writer_);
}

void Agent::initialize() noexcept {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return;
    }

    // Resolve the real allocator before tracing starts, so the first traced
    // allocation is not also the one triggering the dlsym recursion dance.
    real::resolveAll();

    const char* tracePath = std::getenv(ipc::kEnvTraceFile);
    if (tracePath == nullptr || *tracePath == '\0') {
        // Preloaded without a trace target: stay completely passive. This is
        // what makes the library harmless if it is left in LD_PRELOAD.
        return;
    }

    verbose_ = std::getenv(ipc::kEnvVerbose) != nullptr;

    // Are we the process the host launched, or something it exec'd?
    //
    // Every environment variable survives exec, so a child the target spawns
    // arrives here with the same LEAKHUNTER_TRACE path and the same
    // LEAKHUNTER_PID -- but a different getpid(). If we did not stop here, the
    // child would open that path with O_TRUNC and destroy whatever the parent
    // had already flushed; the host would then read the child's header, the
    // child's records and the child's end marker, and report all of it as the
    // target's own. Silently, with droppedRecords still zero.
    if (const char* tracedPid = std::getenv(ipc::kEnvTracedPid); tracedPid != nullptr) {
        const long expectedPid = std::strtol(tracedPid, nullptr, 10);
        if (expectedPid > 0 && expectedPid != static_cast<long>(::getpid())) {
            if (verbose_) {
                char note[192];
                const int length = std::snprintf(
                    note, sizeof(note),
                    "[leakhunter agent] pid %d is a child of the traced process %ld; "
                    "not tracing (see docs/USAGE.md, multi-process targets)\n",
                    static_cast<int>(::getpid()), expectedPid);
                if (length > 0) {
                    diagnostic(note);
                }
            }
            return;
        }
    }

    if (const char* frames = std::getenv(ipc::kEnvMaxFrames); frames != nullptr) {
        const long parsed = std::strtol(frames, nullptr, 10);
        if (parsed > 0 && parsed <= static_cast<long>(ipc::kMaxFrames)) {
            maxFrames_ = static_cast<std::uint16_t>(parsed);
        }
    }

    startTimestampNs_ = monotonicNanos();

    if (!writer_.open(tracePath)) {
        diagnostic("[leakhunter agent] cannot open the trace file; tracing disabled\n");
        return;
    }

    writeFileHeader();
    writeModuleMap();
    installForkHandlers();
    installCrashHandlers();

    active_.store(true, std::memory_order_release);

    if (verbose_) {
        char banner[256];
        const int length =
            std::snprintf(banner, sizeof(banner),
                          "[leakhunter agent] attached to pid %d, unwinder=%s, maxFrames=%u\n",
                          static_cast<int>(::getpid()), unwinderName(),
                          static_cast<unsigned>(maxFrames_));
        if (length > 0) {
            diagnostic(banner);
        }
    }
}

void Agent::emergencyFlush() noexcept {
    // No end marker: its absence is precisely how the host learns the data is
    // partial, and writing one here would claim a clean shutdown that did not
    // happen. Symbols are lost too -- dladdr is not async-signal-safe -- so the
    // host falls back to raw addresses, which still identify the call sites.
    writer_.flushFromSignal();
}

namespace {

/// Signals that end a process without running library destructors.
constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTERM, SIGINT};

/// Flush, then let the signal do what it was going to do anyway.
///
/// SA_RESETHAND has already restored the default disposition by the time this
/// runs, so re-raising reproduces the original outcome exactly: same exit
/// status, same core dump. The target's observable behaviour is unchanged.
extern "C" void onFatalSignal(int signalNumber) {
    Agent::instance().emergencyFlush();
    ::raise(signalNumber);
}

}  // namespace

void Agent::installCrashHandlers() noexcept {
    struct sigaction action {};
    action.sa_handler = &onFatalSignal;
    action.sa_flags = SA_RESETHAND | SA_NODEFER;
    ::sigemptyset(&action.sa_mask);

    // Installed from the library constructor, i.e. before the program's own
    // static initialisers and before main. Any handler the target installs
    // later replaces ours -- which is the right precedence: we would rather
    // lose the flush than change how the program handles its own signals.
    for (const int signalNumber : kFatalSignals) {
        ::sigaction(signalNumber, &action, nullptr);
    }
}

void Agent::installForkHandlers() noexcept {
    static std::atomic<bool> installed{false};
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true)) {
        return;
    }

    // Holding the writer lock across fork() stops a child from inheriting a
    // mutex that is locked forever by a thread that does not exist there.
    //
    // The child then stops tracing altogether: two processes appending partial
    // buffers to one trace file would corrupt it, and stitching multi-process
    // traces together is a feature of its own (see docs/ROADMAP.md).
    //
    // Clearing `active_` is necessary but nowhere near sufficient. The child
    // also inherits a copy of the parent's *unflushed buffer* and the same open
    // file description -- hence the same file offset -- so anything that flushes
    // in the child duplicates the parent's records and shifts the parent's next
    // write. abandonInChild() is what actually severs the connection; see its
    // comment for what this looked like before.
    ::pthread_atfork(
        []() noexcept { Agent::instance().writer_.lock(); },
        []() noexcept { Agent::instance().writer_.unlock(); },
        []() noexcept {
            Agent& agent = Agent::instance();
            agent.active_.store(false, std::memory_order_release);
            agent.writer_.abandonInChild();
            agent.writer_.unlock();
        });
}

void Agent::recordAllocation(void* address, std::size_t size, ipc::AllocKind kind) noexcept {
    if (!active() || address == nullptr) {
        return;
    }

    totalAllocations_.fetch_add(1, std::memory_order_relaxed);
    totalBytesAllocated_.fetch_add(size, std::memory_order_relaxed);

    // Stack buffer: kMaxFrames is 128, so at most 1 KiB. The heap is a
    // recursion hazard and thread-local storage would grow every thread.
    std::uint64_t frames[ipc::kMaxFrames];
    const std::size_t frameCount = captureStack(frames, maxFrames_, kSkipFrames);

    if (frameCount >= maxFrames_) {
        truncatedTraces_.fetch_add(1, std::memory_order_relaxed);
    }

    for (std::size_t i = 0; i < frameCount; ++i) {
        programCounters_.insert(frames[i]);
    }

    ipc::RecordHeader header{};
    header.type = static_cast<std::uint8_t>(ipc::RecordType::Allocation);
    header.kind = static_cast<std::uint8_t>(kind);
    header.extra = static_cast<std::uint16_t>(frameCount);
    header.payloadBytes =
        static_cast<std::uint32_t>(sizeof(ipc::AllocationRecord) + frameCount * sizeof(frames[0]));

    ipc::AllocationRecord record{};
    record.address = reinterpret_cast<std::uint64_t>(address);
    record.size = size;
    record.timestampNs = monotonicNanos();
    record.threadId = currentThreadId();

    const TraceWriter::Chunk chunks[] = {
        {&header, sizeof(header)},
        {&record, sizeof(record)},
        {frames, frameCount * sizeof(frames[0])},
    };
    writer_.appendRecord(chunks, 3);
}

void Agent::recordDeallocation(void* address, ipc::FreeKind kind) noexcept {
    if (!active() || address == nullptr) {
        return;
    }

    totalDeallocations_.fetch_add(1, std::memory_order_relaxed);

    ipc::RecordHeader header{};
    header.type = static_cast<std::uint8_t>(ipc::RecordType::Deallocation);
    header.kind = static_cast<std::uint8_t>(kind);
    header.extra = 0;
    header.payloadBytes = static_cast<std::uint32_t>(sizeof(ipc::DeallocationRecord));

    ipc::DeallocationRecord record{};
    record.address = reinterpret_cast<std::uint64_t>(address);
    record.timestampNs = monotonicNanos();
    record.threadId = currentThreadId();

    const TraceWriter::Chunk chunks[] = {
        {&header, sizeof(header)},
        {&record, sizeof(record)},
    };
    writer_.appendRecord(chunks, 2);
}

void Agent::writeSymbolTable() noexcept {
    std::size_t emitted = 0;

    programCounters_.forEach([&](std::uint64_t programCounter) {
        Dl_info info{};
        const int found = ::dladdr(reinterpret_cast<void*>(programCounter), &info);

        const char* function = found != 0 && info.dli_sname != nullptr ? info.dli_sname : "";
        const char* module = found != 0 && info.dli_fname != nullptr ? info.dli_fname : "";

        // Names are length-prefixed with 16 bits; anything longer is a
        // pathological mangled name and gets clipped rather than corrupting
        // the stream.
        const std::size_t functionBytes = std::min<std::size_t>(std::strlen(function), 0xFFFFU);
        const std::size_t moduleBytes = std::min<std::size_t>(std::strlen(module), 0xFFFFU);

        ipc::SymbolRecord record{};
        record.programCounter = programCounter;
        record.moduleBase = found != 0 ? reinterpret_cast<std::uint64_t>(info.dli_fbase) : 0;
        record.symbolAddress = found != 0 ? reinterpret_cast<std::uint64_t>(info.dli_saddr) : 0;
        record.functionBytes = static_cast<std::uint16_t>(functionBytes);
        record.moduleBytes = static_cast<std::uint16_t>(moduleBytes);

        ipc::RecordHeader header{};
        header.type = static_cast<std::uint8_t>(ipc::RecordType::Symbol);
        header.kind = 0;
        header.extra = 0;
        header.payloadBytes =
            static_cast<std::uint32_t>(sizeof(record) + functionBytes + moduleBytes);

        const TraceWriter::Chunk chunks[] = {
            {&header, sizeof(header)},
            {&record, sizeof(record)},
            {function, functionBytes},
            {module, moduleBytes},
        };
        writer_.appendRecord(chunks, 4);
        ++emitted;
    });

    if (verbose_) {
        char message[128];
        const int length = std::snprintf(message, sizeof(message),
                                         "[leakhunter agent] resolved %zu call sites\n", emitted);
        if (length > 0) {
            diagnostic(message);
        }
    }
}

void Agent::writeEndRecord() noexcept {
    ipc::EndRecord record{};
    record.totalAllocations = totalAllocations_.load(std::memory_order_relaxed);
    record.totalDeallocations = totalDeallocations_.load(std::memory_order_relaxed);
    record.totalBytesAllocated = totalBytesAllocated_.load(std::memory_order_relaxed);
    // Sizes are not known at free() time without a live map inside the target,
    // which is exactly the state we moved to the host. The host derives this
    // from matched allocate/free pairs.
    record.totalBytesFreed = 0;
    record.droppedRecords = writer_.droppedRecords() + programCounters_.overflowed();
    record.truncatedTraces = truncatedTraces_.load(std::memory_order_relaxed);
    record.endTimestampNs = monotonicNanos();

    ipc::RecordHeader header{};
    header.type = static_cast<std::uint8_t>(ipc::RecordType::End);
    header.kind = 0;
    header.extra = 0;
    header.payloadBytes = static_cast<std::uint32_t>(sizeof(record));

    const TraceWriter::Chunk chunks[] = {
        {&header, sizeof(header)},
        {&record, sizeof(record)},
    };
    writer_.appendRecord(chunks, 2);
}

void Agent::shutdown() noexcept {
    if (!active_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // Tracing is off from here on, so dladdr() is free to allocate: those
    // allocations still reach our hooks, find the agent inactive, and pass
    // straight through to the real allocator.
    writeModuleMap();  // pick up anything dlopen'd since start-up
    writeSymbolTable();
    writeEndRecord();
    writer_.close();

    reportWriteFailure();
}

void Agent::reportWriteFailure() noexcept {
    const int failure = writer_.lastWriteErrno();
    if (failure == 0) {
        return;
    }

    // Always on stderr, not only under --verbose: the trace is incomplete and
    // the host cannot work out why on its own. EBADF in particular means the
    // target closed the descriptor out from under us, which the host would
    // otherwise report as "the program may be statically linked" -- confidently
    // and wrongly.
    char note[320];
    const char* explanation =
        failure == EBADF
            ? "the target closed the trace descriptor (the `for (fd = 3; fd < N; ++fd) close(fd)` "
              "idiom does this). Records after that point are lost."
            : "writing the trace failed. The disk may be full.";

    const int length =
        std::snprintf(note, sizeof(note), "[leakhunter agent] %s (errno %d: %s)\n", explanation,
                      failure, std::strerror(failure));
    if (length > 0) {
        diagnostic(note);
    }
}

}  // namespace leakhunter::agent

// ---------------------------------------------------------------------------
// Library lifecycle
//
// Priority 101 is the earliest a user library may request. Constructors run in
// ascending priority and destructors in descending order, so the agent starts
// before the program's own static initialisers and stops after their
// destructors -- everything in between is observed.
// ---------------------------------------------------------------------------

extern "C" __attribute__((constructor(101))) void leakhunterAgentStart() {
    leakhunter::agent::Agent::instance().initialize();
}

extern "C" __attribute__((destructor(101))) void leakhunterAgentStop() {
    leakhunter::agent::Agent::instance().shutdown();
}
