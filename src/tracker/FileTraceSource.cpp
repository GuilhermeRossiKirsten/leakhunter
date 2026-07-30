#include "leakhunter/tracker/FileTraceSource.hpp"

#include <fstream>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "leakhunter/core/Logger.hpp"
#include "leakhunter/ipc/TraceFormat.hpp"

namespace leakhunter::tracker {
namespace {

/// Sequential reader over the trace. Every read is bounds-checked: the file is
/// written by a process that may have been killed mid-record, so a short read
/// is an expected outcome rather than a bug.
class TraceStream {
public:
    explicit TraceStream(std::ifstream& input) : input_(input) {}

    template <typename T>
    [[nodiscard]] bool readPod(T& out) {
        static_assert(std::is_trivially_copyable_v<T>);
        input_.read(reinterpret_cast<char*>(&out), sizeof(T));
        return input_.gcount() == static_cast<std::streamsize>(sizeof(T));
    }

    [[nodiscard]] bool readBytes(void* destination, std::size_t count) {
        if (count == 0) {
            return true;
        }
        input_.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
        return input_.gcount() == static_cast<std::streamsize>(count);
    }

    [[nodiscard]] bool skip(std::size_t count) {
        if (count == 0) {
            return true;
        }
        scratch_.resize(count);
        return readBytes(scratch_.data(), count);
    }

private:
    std::ifstream& input_;
    std::vector<char> scratch_;
};

}  // namespace

FileTraceSource::FileTraceSource(std::filesystem::path path) : path_(std::move(path)) {}

Status FileTraceSource::replay(ITraceVisitor& visitor) {
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return Error{fmt::format("cannot open trace file '{}'", path_.string())};
    }

    // A 1 MiB buffer keeps the syscall count low on multi-million-record traces.
    std::vector<char> streamBuffer(1U << 20);
    input.rdbuf()->pubsetbuf(streamBuffer.data(),
                             static_cast<std::streamsize>(streamBuffer.size()));

    TraceStream stream(input);

    ipc::FileHeader header{};
    if (!stream.readPod(header)) {
        // Three causes, and we genuinely cannot tell them apart from here -- so
        // list them instead of picking one. If the agent managed to say
        // something on stderr (an EBADF from a target that closed our
        // descriptor, say), that message is the authoritative one and it has
        // already been printed above this.
        return Error{fmt::format(
            "trace file '{}' is empty -- no allocation data reached it. Either the target is "
            "statically linked (nothing for LD_PRELOAD to interpose on), or it bypasses the libc "
            "allocator, or it closed the trace descriptor before anything was flushed. Any agent "
            "message printed above this line takes precedence.",
            path_.string())};
    }
    if (header.magic != ipc::kMagic) {
        return Error{fmt::format("'{}' is not a LeakHunter trace (bad magic 0x{:08x})",
                                 path_.string(), header.magic)};
    }
    if (header.version != ipc::kFormatVersion) {
        return Error{fmt::format("trace format version {} is not supported (this build expects {})",
                                 header.version, ipc::kFormatVersion)};
    }
    if (header.pointerSize != sizeof(void*)) {
        return Error{fmt::format("trace came from a {}-bit process but the host is {}-bit",
                                 header.pointerSize * 8, sizeof(void*) * 8)};
    }

    TraceInfo info;
    info.pid = header.pid;
    info.startTimestampNs = header.startTimestampNs;
    info.formatVersion = header.version;
    info.maxFrames = header.maxFrames;
    visitor.onTraceBegin(info);

    TraceSummary summary;
    bool sawEnd = false;
    bool truncated = false;
    bool streaming = true;

    std::vector<std::uint64_t> frames;
    std::vector<char> text;
    ipc::RecordHeader recordHeader{};

    while (streaming && stream.readPod(recordHeader)) {
        switch (static_cast<ipc::RecordType>(recordHeader.type)) {
            case ipc::RecordType::Allocation: {
                ipc::AllocationRecord record{};
                const std::size_t frameCount = recordHeader.extra;

                if (frameCount > ipc::kMaxFrames) {
                    return Error{fmt::format("corrupt trace: frame count {} exceeds the limit of {}",
                                             frameCount, ipc::kMaxFrames)};
                }

                frames.resize(frameCount);
                if (!stream.readPod(record) ||
                    !stream.readBytes(frames.data(), frameCount * sizeof(std::uint64_t))) {
                    truncated = true;
                    streaming = false;
                    break;
                }

                AllocationInfo allocation;
                allocation.address = record.address;
                allocation.size = record.size;
                allocation.timestampNs = record.timestampNs;
                allocation.threadId = record.threadId;
                allocation.kind = static_cast<AllocationKind>(recordHeader.kind);
                allocation.callStack = frames;

                ++summary.totalAllocations;
                summary.totalBytesAllocated += record.size;
                visitor.onAllocation(std::move(allocation));
                break;
            }

            case ipc::RecordType::Deallocation: {
                ipc::DeallocationRecord record{};
                if (!stream.readPod(record)) {
                    truncated = true;
                    streaming = false;
                    break;
                }
                ++summary.totalDeallocations;
                visitor.onDeallocation(record.address, record.timestampNs,
                                       static_cast<ReleaseKind>(recordHeader.kind),
                                       record.threadId);
                break;
            }

            case ipc::RecordType::Symbol: {
                ipc::SymbolRecord record{};
                if (!stream.readPod(record)) {
                    truncated = true;
                    streaming = false;
                    break;
                }

                RawSymbol symbol;
                symbol.programCounter = record.programCounter;
                symbol.moduleBase = record.moduleBase;
                symbol.symbolAddress = record.symbolAddress;

                text.resize(record.functionBytes);
                if (!stream.readBytes(text.data(), record.functionBytes)) {
                    truncated = true;
                    streaming = false;
                    break;
                }
                symbol.mangledFunction.assign(text.data(), record.functionBytes);

                text.resize(record.moduleBytes);
                if (!stream.readBytes(text.data(), record.moduleBytes)) {
                    truncated = true;
                    streaming = false;
                    break;
                }
                symbol.module.assign(text.data(), record.moduleBytes);

                visitor.onSymbol(std::move(symbol));
                break;
            }

            case ipc::RecordType::Module: {
                ipc::ModuleRecord record{};
                if (!stream.readPod(record)) {
                    truncated = true;
                    streaming = false;
                    break;
                }

                text.resize(record.pathBytes);
                if (!stream.readBytes(text.data(), record.pathBytes)) {
                    truncated = true;
                    streaming = false;
                    break;
                }

                ModuleRange module;
                module.base = record.base;
                module.span = record.span;
                module.path.assign(text.data(), record.pathBytes);
                visitor.onModule(std::move(module));
                break;
            }

            case ipc::RecordType::End: {
                ipc::EndRecord record{};
                if (stream.readPod(record)) {
                    summary.droppedRecords = record.droppedRecords;
                    summary.truncatedTraces = record.truncatedTraces;
                    summary.endTimestampNs = record.endTimestampNs;
                    sawEnd = true;
                }
                streaming = false;
                break;
            }

            default:
                // Forward compatibility: unknown record types are skipped using
                // the payload size the writer stored for exactly this purpose.
                if (!stream.skip(recordHeader.payloadBytes)) {
                    streaming = false;
                }
                break;
        }
    }

    if (!sawEnd) {
        // The target crashed, called _exit(), or was killed. Everything read so
        // far is still valid, so flag it and report what we have.
        log::warn(
            "trace has no end marker -- the target did not shut down cleanly, results may be "
            "incomplete");
        summary.droppedRecords = summary.droppedRecords == 0 ? 1 : summary.droppedRecords;
    }
    if (truncated) {
        log::debug("trace truncated after {} allocation records", summary.totalAllocations);
    }

    visitor.onTraceEnd(summary);
    return {};
}

}  // namespace leakhunter::tracker
