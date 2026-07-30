/// The full C allocator surface: malloc, calloc, realloc, aligned_alloc.
///
/// Expected: 3 leaks -- the calloc block, the realloc'd block, and the
/// aligned_alloc block. The malloc/free pairs must not be reported, and the
/// realloc chain must be reported once, at its final size (8192), not at every
/// intermediate size.

#include <cstdio>
#include <cstdlib>

namespace {

void* leakCalloc() {
    return std::calloc(64, sizeof(double));  // 512 bytes, zeroed
}

void* leakRealloc() {
    void* block = std::malloc(128);
    block = std::realloc(block, 1024);
    block = std::realloc(block, 8192);  // only this one is still live
    return block;
}

void* leakAligned() {
    return std::aligned_alloc(64, 4096);
}

void balancedMallocFree() {
    for (int i = 0; i < 2000; ++i) {
        void* block = std::malloc(333);
        std::free(block);
    }
}

void reallocThenFree() {
    void* block = std::malloc(16);
    block = std::realloc(block, 4096);
    std::free(block);  // correctly released
}

}  // namespace

int main() {
    void* a = leakCalloc();
    void* b = leakRealloc();
    void* c = leakAligned();

    balancedMallocFree();
    reallocThenFree();

    std::printf("leaked %p, %p, %p\n", a, b, c);
    return 0;
}
