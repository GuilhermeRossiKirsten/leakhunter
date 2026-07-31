/// @file poc7/src/main.cpp
/// @brief The aligned allocators, and the overloads nobody tests.
///
/// `malloc` and `operator new` are interposed by every tool. The rest of the
/// family is where coverage quietly stops: `aligned_alloc`, `posix_memalign`,
/// and the C++17 over-aligned `operator new(size_t, align_val_t)`, which is a
/// *different function* from plain `operator new` and needs its own hook.
///
/// A tool that misses one of these under-reports silently -- the blocks simply
/// never appear. Every count here is fixed by the constants below, so a missing
/// interceptor shows up as a number that does not add up. See docs/VALIDATION.md.

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

// --- the arithmetic --------------------------------------------------------
//
//   aligned_alloc   : 30 x  256 B  =  7,680 B
//   posix_memalign  : 20 x  512 B  = 10,240 B
//   new(align_val_t): 15 x  256 B  =  3,840 B
//   new char[]      : 10 x  128 B  =  1,280 B
//   -------------------------------------------------------------------------
//   TOTAL LEAKED    : 75 blocks    = 23,040 B
//
// Each family also does matched allocate/release work that must NOT be
// reported, so a tool that ignores the release side over-reports instead.

constexpr int kAlignedAllocLeaks = 30;
constexpr std::size_t kAlignedAllocBytes = 256;
constexpr std::size_t kAlignedAllocAlign = 64;  // 256 % 64 == 0, as C requires

constexpr int kPosixMemalignLeaks = 20;
constexpr std::size_t kPosixBytes = 512;
constexpr std::size_t kPosixAlign = 128;

constexpr int kOverAlignedNewLeaks = 15;
constexpr int kNewArrayLeaks = 10;
constexpr std::size_t kNewArrayBytes = 128;

constexpr int kMatchedPairs = 200;  // must contribute nothing to the report

/// A type whose alignment forces the over-aligned operator new overload.
///
/// 64 exceeds __STDCPP_DEFAULT_NEW_ALIGNMENT__ on every target this builds for,
/// which is what makes the compiler emit the align_val_t call rather than the
/// ordinary one.
struct alignas(64) Tile {
    unsigned char bytes[256];
};

static_assert(sizeof(Tile) == 256, "the document's arithmetic assumes 256-byte tiles");
static_assert(alignof(Tile) == 64, "Tile must be over-aligned to reach the align_val_t overload");

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

volatile unsigned char g_sink = 0;

void touch(void* memory, std::size_t bytes) {
    g_escape = memory;
    auto* raw = static_cast<unsigned char*>(memory);
    raw[0] = 0xA5;
    raw[bytes - 1] = 0x5A;
    g_sink = static_cast<unsigned char>(g_sink + raw[0] + raw[bytes - 1]);
}

/// Allocate and release through every family, so the balanced case is covered
/// as well as the leaking one.
void matchedWork() {
    for (int i = 0; i < kMatchedPairs; ++i) {
        void* a = std::aligned_alloc(kAlignedAllocAlign, kAlignedAllocBytes);
        if (a != nullptr) {
            touch(a, kAlignedAllocBytes);
            std::free(a);
        }

        void* p = nullptr;
        if (::posix_memalign(&p, kPosixAlign, kPosixBytes) == 0 && p != nullptr) {
            touch(p, kPosixBytes);
            std::free(p);
        }

        Tile* tile = new Tile;  // over-aligned new, matched with over-aligned delete
        touch(tile, sizeof(Tile));
        delete tile;

        char* array = new char[kNewArrayBytes];
        touch(array, kNewArrayBytes);
        delete[] array;
    }
}

// Each family leaks from its own function, so the four appear as four distinct
// sites. Left inline in main() they would all be blamed on `main` -- correctly,
// since that is the responsible function -- and the report could not say which
// allocator was involved without reading the stack.
//
// `noinline` is load-bearing here, and is a property of this demonstration
// rather than of the tool. At -O1 the compiler inlines all four into main(),
// and LeakHunter resolves to the outermost frame, so the report would show one
// site named `main`. That is a correct answer to "which function do I open";
// it is not the answer this file exists to demonstrate. See docs/VALIDATION.md.

/// LEAK #1: C11 aligned_alloc.
[[gnu::noinline]] void leakAlignedAlloc() {
    for (int i = 0; i < kAlignedAllocLeaks; ++i) {
        void* block = std::aligned_alloc(kAlignedAllocAlign, kAlignedAllocBytes);
        if (block != nullptr) {
            touch(block, kAlignedAllocBytes);
        }
    }
}

/// LEAK #2: POSIX posix_memalign, which returns the block through a parameter
/// rather than the return value -- a shape an interceptor can get wrong.
[[gnu::noinline]] void leakPosixMemalign() {
    for (int i = 0; i < kPosixMemalignLeaks; ++i) {
        void* block = nullptr;
        if (::posix_memalign(&block, kPosixAlign, kPosixBytes) == 0 && block != nullptr) {
            touch(block, kPosixBytes);
        }
    }
}

/// LEAK #3: operator new(size_t, align_val_t) -- a different function from
/// plain operator new, needing its own interception.
[[gnu::noinline]] void leakOverAlignedNew() {
    for (int i = 0; i < kOverAlignedNewLeaks; ++i) {
        Tile* tile = new Tile;
        touch(tile, sizeof(Tile));
    }
}

/// LEAK #4: operator new[].
[[gnu::noinline]] void leakNewArray() {
    for (int i = 0; i < kNewArrayLeaks; ++i) {
        char* array = new char[kNewArrayBytes];
        touch(array, kNewArrayBytes);
    }
}

}  // namespace

int main() {
    std::printf("aligned_family starting\n");

    matchedWork();

    leakAlignedAlloc();
    leakPosixMemalign();
    leakOverAlignedNew();
    leakNewArray();

    constexpr std::size_t expectedBytes =
        static_cast<std::size_t>(kAlignedAllocLeaks) * kAlignedAllocBytes +
        static_cast<std::size_t>(kPosixMemalignLeaks) * kPosixBytes +
        static_cast<std::size_t>(kOverAlignedNewLeaks) * sizeof(Tile) +
        static_cast<std::size_t>(kNewArrayLeaks) * kNewArrayBytes;
    constexpr int expectedBlocks =
        kAlignedAllocLeaks + kPosixMemalignLeaks + kOverAlignedNewLeaks + kNewArrayLeaks;

    static_assert(expectedBytes == 23040, "docs/VALIDATION.md states 23,040 bytes");
    static_assert(expectedBlocks == 75, "docs/VALIDATION.md states 75 blocks");

    std::printf("  %d matched pairs through four allocator families (must not be reported)\n",
                kMatchedPairs);
    std::printf("  expected leak: %d blocks, %zu bytes\n", expectedBlocks, expectedBytes);
    std::printf("aligned_family finished\n");
    return 0;
}
