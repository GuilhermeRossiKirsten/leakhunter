/// Concurrent leaks. Verifies that the interception is thread-safe and that
/// each leak carries the id of the thread that created it.
///
/// Expected: 4 threads x 25 leaks x 512 bytes = 51200 bytes in 100 leaks,
/// spread over 4 distinct thread ids.

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kThreads = 4;
constexpr int kLeaksPerThread = 25;
constexpr std::size_t kBlockSize = 512;

std::mutex g_mutex;
std::vector<void*> g_leaked;

void worker() {
    for (int i = 0; i < kLeaksPerThread; ++i) {
        void* block = std::malloc(kBlockSize);

        // Some traffic that is correctly balanced, to make sure the concurrent
        // free path is exercised too.
        void* temporary = std::malloc(128);
        std::free(temporary);

        const std::lock_guard<std::mutex> lock(g_mutex);
        g_leaked.push_back(block);
    }
}

}  // namespace

int main() {
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::printf("leaked %zu blocks across %d threads\n", g_leaked.size(), kThreads);
    return 0;
}
