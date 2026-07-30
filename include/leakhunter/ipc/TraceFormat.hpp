/// @file TraceFormat.hpp
/// @brief Wire format shared by the injected agent and the host analyser.
///
/// This header is the single point of contact between the two halves of
/// LeakHunter. It is deliberately dependency-free (only <cstdint>) because the
/// agent side runs inside the target process, below the C++ standard library:
/// anything that allocates would re-enter the very hooks we install.
///
/// Layout notes:
///   * The trace file is written by one process and read by another on the
///     same machine, so native endianness and native alignment are assumed.
///   * Every record starts with a RecordHeader carrying `payloadBytes`, which
///     lets a newer host skip records emitted by an older/newer agent instead
///     of failing outright.

#pragma once

#include <cstdint>

namespace leakhunter::ipc {

/// 'L','H','T','R' -- LeakHunter TRace.
inline constexpr std::uint32_t kMagic = 0x4C485452U;

/// Bumped whenever a record layout changes incompatibly.
inline constexpr std::uint32_t kFormatVersion = 1U;

/// Upper bound on frames per allocation, enforced on both sides.
inline constexpr std::uint16_t kMaxFrames = 128U;

/// Environment variables understood by the agent.
inline constexpr char kEnvTraceFile[] = "LEAKHUNTER_TRACE";     ///< output path (required)
inline constexpr char kEnvMaxFrames[] = "LEAKHUNTER_MAX_FRAMES";///< frames captured per allocation
inline constexpr char kEnvVerbose[] = "LEAKHUNTER_VERBOSE";     ///< agent diagnostics on stderr

/// PID of the process the host actually launched.
///
/// Written by the host *after* fork(), so it holds the traced process's own pid.
/// Every environment variable is inherited across exec, so a child the target
/// spawns sees this same value while its own getpid() differs -- which is how
/// the agent knows it is not the process being traced and must stay out of the
/// way. Without it, the child would open the same trace path with O_TRUNC and
/// destroy the parent's records, and the host would then report the child's
/// allocations as if they were the target's.
inline constexpr char kEnvTracedPid[] = "LEAKHUNTER_PID";

enum class RecordType : std::uint8_t {
    Allocation = 1,  ///< AllocationRecord + frameCount * std::uint64_t
    Deallocation = 2,///< DeallocationRecord
    Symbol = 3,      ///< SymbolRecord + function bytes + module bytes
    End = 4,         ///< EndRecord, always the last record of a healthy trace
    Module = 5,      ///< ModuleRecord + path bytes
};

/// Which entry point produced an allocation.
enum class AllocKind : std::uint8_t {
    Malloc = 0,
    Calloc = 1,
    Realloc = 2,
    AlignedAlloc = 3,
    New = 4,
    NewArray = 5,
    Unknown = 255,
};

/// Which entry point released a block. Paired with the AllocKind recorded for
/// the same address, this is what makes `new[]` released by `free()` visible.
///
/// `Unknown` is 255 on purpose. Agents up to and including the version that
/// introduced this enum wrote `AllocKind::Unknown` -- also 255 -- into the
/// deallocation `kind` byte, so an older trace decodes as "we do not know how
/// this was freed" rather than as a stream of bogus `free()` claims. That is
/// what keeps the format version at 1 across this addition.
enum class FreeKind : std::uint8_t {
    Free = 0,
    Delete = 1,
    DeleteArray = 2,
    Realloc = 3,
    Unknown = 255,
};

#pragma pack(push, 1)

/// Written once, at agent start-up.
struct FileHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t pid;
    std::uint64_t startTimestampNs;
    std::uint16_t pointerSize;
    std::uint16_t maxFrames;
    std::uint32_t reserved;
};

/// Precedes every record. `extra` is record-specific (frame count for
/// allocations); `payloadBytes` counts the bytes that follow this header.
///
/// `kind` is interpreted per record type: AllocKind for Allocation records,
/// FreeKind for Deallocation records, and unused (0) for everything else.
struct RecordHeader {
    std::uint8_t type;
    std::uint8_t kind;
    std::uint16_t extra;
    std::uint32_t payloadBytes;
};

struct AllocationRecord {
    std::uint64_t address;
    std::uint64_t size;
    std::uint64_t timestampNs;
    std::uint64_t threadId;
    // Followed by RecordHeader::extra program counters (std::uint64_t each).
};

struct DeallocationRecord {
    std::uint64_t address;
    std::uint64_t timestampNs;
    std::uint64_t threadId;
};

/// Emitted during shutdown, once per distinct program counter observed. The
/// agent resolves these with dladdr() while still inside the target process,
/// where the dynamic loader can map an address back to a module.
struct SymbolRecord {
    std::uint64_t programCounter;
    std::uint64_t moduleBase;    ///< load address of the containing object
    std::uint64_t symbolAddress; ///< start of the containing symbol, 0 if unknown
    std::uint16_t functionBytes; ///< mangled name length, no terminator
    std::uint16_t moduleBytes;   ///< module path length, no terminator
    std::uint32_t reserved;
    // Followed by functionBytes + moduleBytes raw chars.
};

/// One loaded object and the address range it occupies.
///
/// Written at start-up, *before* any allocation record, and again at shutdown
/// to pick up anything dlopen'd in between. Emitting it early is what makes a
/// crashed program's trace useful: the symbol table below is produced during
/// shutdown and is therefore lost on a crash, but with the module map the host
/// can still turn every raw address into (module, offset) and hand that to a
/// symbolizer.
struct ModuleRecord {
    std::uint64_t base;   ///< load address
    std::uint64_t span;   ///< bytes covered by the object's loadable segments
    std::uint16_t pathBytes;
    std::uint16_t reserved;
    std::uint32_t reserved2;
    // Followed by pathBytes raw chars.
};

/// Agent-side counters, used to cross-check what the host reconstructed.
struct EndRecord {
    std::uint64_t totalAllocations;
    std::uint64_t totalDeallocations;
    std::uint64_t totalBytesAllocated;
    std::uint64_t totalBytesFreed;
    std::uint64_t droppedRecords;    ///< non-zero means the trace is lossy
    std::uint64_t truncatedTraces;   ///< stacks that hit the frame limit
    std::uint64_t endTimestampNs;
};

#pragma pack(pop)

static_assert(sizeof(FileHeader) == 32, "FileHeader layout changed");
static_assert(sizeof(RecordHeader) == 8, "RecordHeader layout changed");
static_assert(sizeof(AllocationRecord) == 32, "AllocationRecord layout changed");
static_assert(sizeof(DeallocationRecord) == 24, "DeallocationRecord layout changed");
static_assert(sizeof(SymbolRecord) == 32, "SymbolRecord layout changed");
static_assert(sizeof(ModuleRecord) == 24, "ModuleRecord layout changed");
static_assert(sizeof(EndRecord) == 56, "EndRecord layout changed");

}  // namespace leakhunter::ipc
