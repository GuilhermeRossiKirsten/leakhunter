/// @file service.cpp
/// @brief A long-running service that leaks a little on every tick.
///
/// Deliberately simple: one loop, one leak, no cleverness. The point is not the
/// bug -- it is that the program **never exits on its own**. You stop it, and
/// LeakHunter still tells you what it found.
///
/// That is the shape of the thing people actually want to point a leak detector
/// at: a daemon, a server, a worker that has been up for three days and is
/// slowly growing.
///
/// Expected: one leak site, `handleRequest`, growing by 512 bytes per tick.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <unistd.h>

namespace {

constexpr std::size_t kRequestBytes = 512;
constexpr auto kTickInterval = std::chrono::milliseconds(200);

/// Published so the allocation is observable and cannot be elided. See
/// docs/CONTRIBUTING.md -- a compiler is allowed to delete an allocation whose
/// result nothing can see, and both GCC and Clang do.
void* volatile g_lastRequest = nullptr;

/// Handles one request. The buffer is never released: this is the bug.
///
/// `noinline` for the demonstration only: called once from a loop this small, a
/// compiler folds it into main() and the report then blames `main` at the right
/// file and line. Correct, but it reads badly. Real request handlers are not
/// three lines long.
__attribute__((noinline)) void handleRequest(unsigned long tick) {
    auto* request = static_cast<char*>(std::malloc(kRequestBytes));
    if (request == nullptr) {
        return;
    }
    g_lastRequest = request;

    std::snprintf(request, kRequestBytes, "request-%lu", tick);

    // ... the work that would happen here ...
    //
    // and no free(request). One 512-byte block per tick is nothing; a week of
    // uptime at five ticks a second is 1.5 GiB.
}

/// The same work, done correctly, so the report has something to *not* mention.
__attribute__((noinline)) void handleRequestProperly(unsigned long tick) {
    auto* request = static_cast<char*>(std::malloc(kRequestBytes));
    if (request == nullptr) {
        return;
    }
    g_lastRequest = request;
    std::snprintf(request, kRequestBytes, "healthcheck-%lu", tick);
    std::free(request);
}

}  // namespace

int main() {
    std::printf("service up (pid %d) -- stop it with Ctrl-C, or: kill %d\n",
                static_cast<int>(::getpid()), static_cast<int>(::getpid()));
    std::fflush(stdout);

    for (unsigned long tick = 1;; ++tick) {
        handleRequest(tick);

        // Three correct requests for every leaked one.
        for (int i = 0; i < 3; ++i) {
            handleRequestProperly(tick);
        }

        if (tick % 5 == 0) {
            std::printf("  tick %lu -- %lu KiB leaked so far\n", tick,
                        (tick * kRequestBytes) / 1024);
            std::fflush(stdout);
        }

        std::this_thread::sleep_for(kTickInterval);
    }
}
