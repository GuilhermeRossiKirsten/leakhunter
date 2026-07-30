/// Several leaks from several call sites, plus correctly freed memory to prove
/// the tool does not report false positives.
///
/// Expected: 3 distinct leak groups.
///   * leakSmall   -- 100 x 64 bytes   =  6400 bytes
///   * leakMedium  --  10 x 4096 bytes = 40960 bytes
///   * leakOne     --   1 x 1 MiB
/// `allocateAndFree` must not appear at all.

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

std::vector<void*> g_keepAlive;

void leakSmall() {
    for (int i = 0; i < 100; ++i) {
        g_keepAlive.push_back(std::malloc(64));
    }
}

void leakMedium() {
    for (int i = 0; i < 10; ++i) {
        g_keepAlive.push_back(std::malloc(4096));
    }
}

void leakOne() {
    g_keepAlive.push_back(std::malloc(1024 * 1024));
}

void allocateAndFree() {
    for (int i = 0; i < 5000; ++i) {
        void* block = std::malloc(256);
        std::free(block);
    }
}

}  // namespace

int main() {
    leakSmall();
    leakMedium();
    leakOne();
    allocateAndFree();

    std::printf("leaked %zu blocks on purpose\n", g_keepAlive.size());

    // g_keepAlive keeps the pointers reachable, but nothing ever frees them:
    // this is exactly the "still reachable at exit" shape of a real leak.
    return 0;
}
