/// @file poc9/src/main.cpp
/// @brief Eight threads leaking at once -- attribution and arithmetic under load.
///
/// Written in C++ throughout: `new char[]` for the leaks, `std::make_unique` for
/// the churn. The C allocator family is validated by poc6 and poc7 instead,
/// which is where it belongs -- see the coverage table in docs/VALIDATION.md.
///
/// A registry that is merely *not crashy* under concurrency can still be wrong:
/// blocks attributed to the thread that happened to be scheduled, counts drifting
/// because two threads raced, a peak that never saw the simultaneous high-water
/// mark. None of that shows on a single-threaded demonstration.
///
/// Every thread here does exactly the same, exactly known work, so the totals
/// are a multiplication rather than an observation. See docs/VALIDATION.md.

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

// --- the arithmetic --------------------------------------------------------
//
//   leaks : 8 threads x 125 blocks x 128 B  = 128,000 B in 1,000 blocks
//   churn : 8 threads x 500 blocks x 256 B  = 1,024,000 B allocated and freed
//   --------------------------------------------------------------------------
//   TOTAL LEAKED  : 1,000 blocks = 128,000 B, reached from 8 threads
//
// The churn leaks nothing and exists to be visible in "where the memory went":
// 1 MiB through the allocator that no leak report would ever mention.

constexpr int kThreads = 8;
constexpr int kLeaksPerThread = 125;
constexpr std::size_t kLeakBytes = 128;

constexpr int kChurnPerThread = 500;
constexpr std::size_t kChurnBytes = 256;

/// Where the pointer itself escapes to, so the allocation cannot be elided.
///
/// Writing through a block is *not* enough. Both compilers may delete an
/// allocation whose result is unobservable -- C++ explicitly permits it
/// (N3664) -- and Clang folds the write-then-read back to a constant and
/// removes the call. Storing the pointer in a volatile makes it escape, which
/// forces the allocation to exist.
///
/// Found the hard way: without this, Clang dropped 25 of poc6's leaks and all
/// of poc8's, and the reports were correct about a program that no longer did
/// what its source said.
volatile void* g_escape = nullptr;

std::atomic<unsigned> g_sink{0};

void touch(void* memory, std::size_t bytes) {
    g_escape = memory;
    auto* raw = static_cast<unsigned char*>(memory);
    raw[0] = 0x33;
    raw[bytes - 1] = 0x44;
    g_sink.fetch_add(raw[0] + raw[bytes - 1], std::memory_order_relaxed);
}

/// One worker: leaks a fixed number of blocks, and recycles a fixed number.
///
/// The two are interleaved on purpose. A tracker that handles them in separate
/// phases would not be tested by separate phases.
void worker() {
    std::vector<char*> kept;
    kept.reserve(kLeaksPerThread);

    for (int i = 0; i < kLeaksPerThread; ++i) {
        // LEAK: 125 blocks per thread, never released. `new char[]` rather than
        // malloc, so this exercises operator new[] and its matching delete[].
        char* leaked = new char[kLeakBytes];
        touch(leaked, kLeakBytes);
        kept.push_back(leaked);

        // ...and four recycled blocks between each, held by unique_ptr so the
        // release is automatic. These must not be reported.
        for (int j = 0; j < kChurnPerThread / kLeaksPerThread; ++j) {
            auto temporary = std::make_unique<char[]>(kChurnBytes);
            touch(temporary.get(), kChurnBytes);
        }
    }

    // `kept` goes out of scope holding raw pointers. The vector's own storage is
    // released; the 125 blocks it pointed at are not, which is the leak.
    for (char* block : kept) {
        g_sink.fetch_add(static_cast<unsigned char>(*block), std::memory_order_relaxed);
    }
}

}  // namespace

int main() {
    std::printf("thread_storm starting\n");

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back(worker);
    }
    for (std::thread& thread : workers) {
        thread.join();
    }

    constexpr std::size_t expectedBytes =
        static_cast<std::size_t>(kThreads) * kLeaksPerThread * kLeakBytes;
    constexpr int expectedBlocks = kThreads * kLeaksPerThread;
    constexpr std::size_t churnBytes =
        static_cast<std::size_t>(kThreads) * kChurnPerThread * kChurnBytes;

    static_assert(expectedBytes == 128000, "docs/VALIDATION.md states 128,000 bytes");
    static_assert(expectedBlocks == 1000, "docs/VALIDATION.md states 1,000 blocks");
    static_assert(churnBytes == 1024000, "docs/VALIDATION.md states 1,024,000 bytes of churn");

    std::printf("  %d threads finished\n", kThreads);
    std::printf("  churn (allocated and released): %zu bytes -- no leak, but real traffic\n",
                churnBytes);
    std::printf("  expected leak: %d blocks, %zu bytes, across %d threads\n", expectedBlocks,
                expectedBytes, kThreads);
    std::printf("thread_storm finished\n");
    return 0;
}
