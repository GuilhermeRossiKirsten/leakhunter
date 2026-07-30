/// The agent/host wire format. These tests write a trace by hand -- exactly as
/// the agent would -- and read it back through the real FileTraceSource, so a
/// layout change on either side breaks here rather than in production.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "TestFramework.hpp"
#include "leakhunter/ipc/TraceFormat.hpp"
#include "leakhunter/registry/AllocationRegistry.hpp"
#include "leakhunter/symbols/SymbolResolver.hpp"
#include "leakhunter/tracker/FileTraceSource.hpp"
#include "leakhunter/tracker/MemoryTracker.hpp"

namespace fs = std::filesystem;
namespace ipc = leakhunter::ipc;

namespace {

/// Minimal stand-in for the agent's TraceWriter.
class TraceBuilder {
public:
    explicit TraceBuilder(fs::path path) : path_(std::move(path)) {
        ipc::FileHeader header{};
        header.magic = ipc::kMagic;
        header.version = ipc::kFormatVersion;
        header.pid = 4242;
        header.startTimestampNs = 1000;
        header.pointerSize = sizeof(void*);
        header.maxFrames = 32;
        append(&header, sizeof(header));
    }

    void allocation(std::uint64_t address, std::uint64_t size,
                    const std::vector<std::uint64_t>& frames, std::uint64_t threadId = 1) {
        ipc::RecordHeader header{};
        header.type = static_cast<std::uint8_t>(ipc::RecordType::Allocation);
        header.kind = static_cast<std::uint8_t>(ipc::AllocKind::Malloc);
        header.extra = static_cast<std::uint16_t>(frames.size());
        header.payloadBytes = static_cast<std::uint32_t>(sizeof(ipc::AllocationRecord) +
                                                         frames.size() * sizeof(std::uint64_t));

        ipc::AllocationRecord record{};
        record.address = address;
        record.size = size;
        record.timestampNs = 2000;
        record.threadId = threadId;

        append(&header, sizeof(header));
        append(&record, sizeof(record));
        append(frames.data(), frames.size() * sizeof(std::uint64_t));
    }

    void deallocation(std::uint64_t address) {
        ipc::RecordHeader header{};
        header.type = static_cast<std::uint8_t>(ipc::RecordType::Deallocation);
        header.payloadBytes = sizeof(ipc::DeallocationRecord);

        ipc::DeallocationRecord record{};
        record.address = address;
        record.timestampNs = 3000;
        record.threadId = 1;

        append(&header, sizeof(header));
        append(&record, sizeof(record));
    }

    void symbol(std::uint64_t programCounter, const std::string& function,
                const std::string& module) {
        ipc::SymbolRecord record{};
        record.programCounter = programCounter;
        record.moduleBase = 0x400000;
        record.symbolAddress = programCounter - 16;
        record.functionBytes = static_cast<std::uint16_t>(function.size());
        record.moduleBytes = static_cast<std::uint16_t>(module.size());

        ipc::RecordHeader header{};
        header.type = static_cast<std::uint8_t>(ipc::RecordType::Symbol);
        header.payloadBytes =
            static_cast<std::uint32_t>(sizeof(record) + function.size() + module.size());

        append(&header, sizeof(header));
        append(&record, sizeof(record));
        append(function.data(), function.size());
        append(module.data(), module.size());
    }

    void module(std::uint64_t base, std::uint64_t span, const std::string& path) {
        ipc::ModuleRecord record{};
        record.base = base;
        record.span = span;
        record.pathBytes = static_cast<std::uint16_t>(path.size());

        ipc::RecordHeader header{};
        header.type = static_cast<std::uint8_t>(ipc::RecordType::Module);
        header.payloadBytes = static_cast<std::uint32_t>(sizeof(record) + path.size());

        append(&header, sizeof(header));
        append(&record, sizeof(record));
        append(path.data(), path.size());
    }

    /// A record type this build does not know about. Readers must skip it
    /// using payloadBytes rather than giving up on the rest of the trace.
    void unknownRecord(std::size_t payloadBytes) {
        ipc::RecordHeader header{};
        header.type = 200;
        header.payloadBytes = static_cast<std::uint32_t>(payloadBytes);

        append(&header, sizeof(header));
        const std::vector<char> filler(payloadBytes, '\xAB');
        append(filler.data(), filler.size());
    }

    void end() {
        ipc::EndRecord record{};
        record.totalAllocations = 0;
        record.endTimestampNs = 9999;

        ipc::RecordHeader header{};
        header.type = static_cast<std::uint8_t>(ipc::RecordType::End);
        header.payloadBytes = sizeof(record);

        append(&header, sizeof(header));
        append(&record, sizeof(record));
    }

    void flush() {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
    }

    /// Simulates a process killed mid-write.
    void flushTruncated(std::size_t keepBytes) {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(bytes_.data(),
                     static_cast<std::streamsize>(std::min(keepBytes, bytes_.size())));
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    void append(const void* data, std::size_t size) {
        const auto* source = static_cast<const char*>(data);
        bytes_.insert(bytes_.end(), source, source + size);
    }

    fs::path path_;
    std::vector<char> bytes_;
};

fs::path temporaryPath(const char* name) {
    return fs::temp_directory_path() / (std::string("leakhunter-test-") + name + ".lhtrace");
}

/// Deletes the trace when the test finishes, pass or fail.
struct TempTrace {
    fs::path path;
    explicit TempTrace(const char* name) : path(temporaryPath(name)) {}
    ~TempTrace() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

}  // namespace

LH_TEST(Trace, roundtrips_allocations_frees_and_symbols) {
    const TempTrace temp("roundtrip");

    TraceBuilder builder(temp.path);
    builder.symbol(0x401000, "allocateBuffer", "/home/dev/app");
    builder.symbol(0x401100, "main", "/home/dev/app");
    builder.allocation(0xAAAA, 1024, {0x401000, 0x401100});
    builder.allocation(0xBBBB, 2048, {0x401000, 0x401100});
    builder.deallocation(0xBBBB);
    builder.end();
    builder.flush();

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    const auto status = tracker.consume(source);
    LH_CHECK(status.hasValue());

    LH_CHECK_EQ(registry.stats().totalAllocations, std::uint64_t{2});
    LH_CHECK_EQ(registry.stats().totalDeallocations, std::uint64_t{1});
    LH_CHECK_EQ(registry.liveCount(), std::size_t{1});
    LH_CHECK_EQ(resolver.size(), std::size_t{2});

    const auto frame = resolver.resolve(0x401000);
    LH_CHECK_EQ(frame.function, std::string{"allocateBuffer"});
    LH_CHECK_EQ(frame.module, std::string{"/home/dev/app"});
    LH_CHECK_EQ(frame.moduleOffset(), std::uint64_t{0x1000});

    const auto live = registry.takeLiveAllocations();
    LH_CHECK_EQ(live.size(), std::size_t{1});
    LH_CHECK_EQ(live[0].address, std::uint64_t{0xAAAA});
    LH_CHECK_EQ(live[0].callStack.size(), std::size_t{2});
}

LH_TEST(Trace, an_empty_file_is_reported_as_an_error) {
    const TempTrace temp("empty");
    { std::ofstream create(temp.path, std::ios::binary | std::ios::trunc); }

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(!tracker.consume(source).hasValue());
}

LH_TEST(Trace, a_missing_file_is_reported_as_an_error) {
    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temporaryPath("does-not-exist"));

    LH_CHECK(!tracker.consume(source).hasValue());
}

LH_TEST(Trace, a_foreign_file_is_rejected) {
    const TempTrace temp("garbage");
    {
        std::ofstream output(temp.path, std::ios::binary | std::ios::trunc);
        output << "this is definitely not a leakhunter trace file, not at all";
    }

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(!tracker.consume(source).hasValue());
}

LH_TEST(Trace, a_truncated_trace_still_yields_the_records_that_made_it) {
    // The target crashed. Everything written before the crash is valid data and
    // must survive; the missing end marker is what flags the result as partial.
    const TempTrace temp("truncated");

    TraceBuilder builder(temp.path);
    builder.allocation(0xAAAA, 1024, {0x401000});
    builder.allocation(0xBBBB, 2048, {0x401000});
    const std::size_t completeSize = builder.size();
    builder.allocation(0xCCCC, 4096, {0x401000});
    builder.end();
    builder.flushTruncated(completeSize + 6);  // half of the third record

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(tracker.consume(source).hasValue());
    LH_CHECK_EQ(registry.stats().totalAllocations, std::uint64_t{2});
    LH_CHECK(registry.stats().droppedRecords > 0);
}

LH_TEST(Trace, symbols_are_demangled) {
    const TempTrace temp("demangle");

    TraceBuilder builder(temp.path);
    builder.symbol(0x401000, "_ZN3app5CacheC1Ev", "/home/dev/app");
    builder.allocation(0xAAAA, 16, {0x401000});
    builder.end();
    builder.flush();

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(tracker.consume(source).hasValue());
    LH_CHECK_EQ(resolver.resolve(0x401000).function, std::string{"app::Cache::Cache()"});
}

LH_TEST(Trace, the_module_map_places_addresses_when_symbols_are_missing) {
    // What a crashed target leaves behind: modules were written at start-up,
    // the dladdr pass at shutdown never ran, and there is no end marker. The
    // addresses must still be attributed to an object, otherwise every leak
    // becomes its own anonymous group and the report is unusable.
    const TempTrace temp("modules-only");

    TraceBuilder builder(temp.path);
    builder.module(0x400000, 0x10000, "/home/dev/app");
    builder.module(0x7F0000000000, 0x200000, "/usr/lib/libc.so.6");
    builder.allocation(0xAAAA, 512, {0x401234, 0x7F0000001111});
    builder.flush();  // no end record: the target died

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(tracker.consume(source).hasValue());

    const auto appFrame = resolver.resolve(0x401234);
    LH_CHECK_EQ(appFrame.module, std::string{"/home/dev/app"});
    LH_CHECK_EQ(appFrame.moduleOffset(), std::uint64_t{0x1234});

    const auto libcFrame = resolver.resolve(0x7F0000001111);
    LH_CHECK_EQ(libcFrame.module, std::string{"/usr/lib/libc.so.6"});
    LH_CHECK_EQ(libcFrame.moduleOffset(), std::uint64_t{0x1111});
}

LH_TEST(Trace, an_address_outside_every_module_stays_unattributed) {
    const TempTrace temp("outside-modules");

    TraceBuilder builder(temp.path);
    builder.module(0x400000, 0x10000, "/home/dev/app");
    builder.allocation(0xAAAA, 512, {0x900000});  // past the end of the range
    builder.end();
    builder.flush();

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(tracker.consume(source).hasValue());

    const auto frame = resolver.resolve(0x900000);
    LH_CHECK(frame.module.empty());
    LH_CHECK(!frame.resolved);
}

LH_TEST(Trace, dladdr_symbols_win_over_the_module_map) {
    const TempTrace temp("symbol-precedence");

    TraceBuilder builder(temp.path);
    builder.module(0x400000, 0x10000, "/home/dev/app");
    builder.symbol(0x401234, "realFunction", "/home/dev/app");
    builder.allocation(0xAAAA, 512, {0x401234});
    builder.end();
    builder.flush();

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(tracker.consume(source).hasValue());
    LH_CHECK_EQ(resolver.resolve(0x401234).function, std::string{"realFunction"});
}

LH_TEST(Trace, unknown_record_types_are_skipped_not_fatal) {
    // Forward compatibility: a trace from a newer agent must still be readable.
    const TempTrace temp("forward-compat");

    TraceBuilder builder(temp.path);
    builder.allocation(0xAAAA, 100, {0x401000});
    builder.unknownRecord(48);
    builder.allocation(0xBBBB, 200, {0x401000});
    builder.end();
    builder.flush();

    leakhunter::registry::AllocationRegistry registry;
    leakhunter::symbols::SymbolResolver resolver;
    leakhunter::tracker::MemoryTracker tracker(registry, resolver);
    leakhunter::tracker::FileTraceSource source(temp.path);

    LH_CHECK(tracker.consume(source).hasValue());
    LH_CHECK_EQ(registry.stats().totalAllocations, std::uint64_t{2});
}

LH_TEST(Trace, plain_c_symbols_pass_through_demangling_unchanged) {
    LH_CHECK_EQ(leakhunter::symbols::SymbolResolver::demangle("malloc"), std::string{"malloc"});
    LH_CHECK_EQ(leakhunter::symbols::SymbolResolver::demangle(""), std::string{});
    LH_CHECK_EQ(leakhunter::symbols::SymbolResolver::demangle("_Znot_really_mangled"),
                std::string{"_Znot_really_mangled"});
}
