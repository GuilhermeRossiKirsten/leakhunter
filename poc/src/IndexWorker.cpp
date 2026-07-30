/// @file IndexWorker.cpp
/// @brief Worker threads -- and bug #4, a scratch buffer leaked per batch.

#include "poc/IndexWorker.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace poc {
namespace {

constexpr std::size_t kScratchBytes = 2048;

/// Every scratch buffer is published through this before being abandoned.
///
/// Without it the allocations here are not observable, and **the compiler is
/// allowed to delete them** -- C++ explicitly permits eliding allocations.
/// Clang at -O1 does exactly that: the same source produced 4829 allocations
/// under GCC and 4329 under Clang, and bug #4 simply did not exist in the Clang
/// build. LeakHunter reported both correctly; the demo was the thing that was
/// wrong.
///
/// A `volatile` store is an observable side effect, so no compiler may remove
/// the allocation that feeds it. Real leaking code is observable for ordinary
/// reasons -- the pointer goes into a container, a struct, a callback -- which
/// is why the other three defects in this demo need no such help.
void* volatile g_publishedScratch = nullptr;

/// Indexes one batch. The scratch buffer is allocated per call.
///
/// `noinline` is here for the demonstration, not for the bug. Called from one
/// place with a body this small, the compiler inlines it into the worker lambda
/// -- and then the frame does not exist, so the report blames
/// `std::thread::_State_impl<...>::_M_run()` at the right file and line. The
/// attribution is still correct, it just reads badly. Real indexing code is
/// called from several places and is far too large to inline; this keeps the
/// demo representative of that rather than of a one-line helper.
__attribute__((noinline)) std::size_t indexBatch(std::size_t batch) {
    auto* scratch = static_cast<unsigned char*>(std::malloc(kScratchBytes));
    if (scratch == nullptr) {
        return 0;
    }
    g_publishedScratch = scratch;

    std::memset(scratch, static_cast<int>(batch & 0xFF), kScratchBytes);

    std::size_t checksum = 0;
    for (std::size_t i = 0; i < kScratchBytes; ++i) {
        checksum += scratch[i];
    }

    // -----------------------------------------------------------------------
    // BUG #4: no free(scratch).
    //
    // Small, per-batch, and on a background thread -- the combination that makes
    // this the hardest kind to notice. A single 2 KiB block is nothing; running
    // for a day at a hundred batches a second is 17 GiB.
    //
    // LeakHunter records the kernel thread id of every allocation, so the report
    // shows this one site spread across every worker rather than as several
    // unrelated leaks.
    // -----------------------------------------------------------------------
    return checksum;
}

/// Correctly balanced work, so the report has something to *not* mention.
void indexBatchProperly(std::size_t batch) {
    auto* scratch = static_cast<unsigned char*>(std::malloc(kScratchBytes));
    if (scratch == nullptr) {
        return;
    }
    g_publishedScratch = scratch;
    std::memset(scratch, static_cast<int>(batch & 0xFF), kScratchBytes);
    std::free(scratch);
}

}  // namespace

std::size_t runIndexer(std::size_t threadCount, std::size_t tasksPerThread) {
    std::atomic<std::size_t> indexed{0};
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    for (std::size_t t = 0; t < threadCount; ++t) {
        workers.emplace_back([t, tasksPerThread, &indexed] {
            for (std::size_t task = 0; task < tasksPerThread; ++task) {
                (void)indexBatch(t * tasksPerThread + task);

                // Four balanced batches for every leaked one: a report that
                // cannot tell these apart is useless.
                for (int repeat = 0; repeat < 4; ++repeat) {
                    indexBatchProperly(task);
                }

                indexed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }
    return indexed.load(std::memory_order_relaxed);
}

}  // namespace poc
