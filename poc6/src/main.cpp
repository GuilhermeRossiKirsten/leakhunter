/// @file poc6/src/main.cpp
/// @brief realloc chains -- the allocator family where double-counting is easy.
///
/// `realloc` is the one entry point that both frees and allocates. A tracker
/// that treats it as an allocation double-counts; one that treats it as a free
/// loses the block. Either mistake is invisible on a program that only ever
/// calls malloc and free, which is why this exists.
///
/// `realloc` has no C++ equivalent, which is exactly why it earns a file: it is
/// the only entry point that frees *and* allocates in one call. C++ code reaches
/// it constantly anyway -- every `std::string` and `std::vector` allocation ends
/// up in `malloc` under `operator new`, and any C library linked in uses it
/// directly.
///
/// The second half does the same growth the C++ way -- `new char[]`, copy,
/// `delete[]` the old -- so both paths are validated side by side and can be
/// compared in the report.
///
/// Every number below is fixed by the constants at the top, so the expected
/// result can be worked out on paper and compared against the report. See
/// docs/VALIDATION.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// --- the arithmetic --------------------------------------------------------
//
//   grown chains   : 100, each malloc(64) -> realloc(256) -> realloc(1024)
//   of those, freed:  60
//   leaked         :  40  x 1024 B  =  40,960 B
//   calloc leaks   :  25  x  8*16   =   3,200 B
//   C++ grow chains:  50, of which 20 leak x 1024 B = 20,480 B
//   ------------------------------------------------------------------------
//   TOTAL LEAKED   :  85 blocks     =  64,640 B
//
// Everything else in this file is deliberately balanced.

constexpr int kChains = 100;
constexpr int kFreedChains = 60;
constexpr int kLeakedChains = kChains - kFreedChains;  // 40

constexpr std::size_t kFirstBytes = 64;
constexpr std::size_t kSecondBytes = 256;
constexpr std::size_t kFinalBytes = 1024;

constexpr int kCallocLeaks = 25;
constexpr std::size_t kCallocCount = 8;
constexpr std::size_t kCallocSize = 16;

constexpr int kCppChains = 50;
constexpr int kCppLeaked = 20;

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

/// Somewhere for the *contents* to go, so the writes are not dead stores either.
volatile unsigned char g_sink = 0;

void touch(void* memory, std::size_t bytes) {
    g_escape = memory;
    auto* raw = static_cast<unsigned char*>(memory);
    raw[0] = static_cast<unsigned char>(bytes & 0xFF);
    raw[bytes - 1] = 0x5A;
    g_sink = static_cast<unsigned char>(g_sink + raw[0] + raw[bytes - 1]);
}

/// malloc -> realloc -> realloc. Only the final block is live afterwards.
///
/// glibc may satisfy a realloc in place and hand back the same pointer, or move
/// the block. Both happen in this loop, and the answer is the same either way:
/// one live block of kFinalBytes per chain.
char* growChain() {
    char* buffer = static_cast<char*>(std::malloc(kFirstBytes));
    if (buffer == nullptr) {
        return nullptr;
    }
    touch(buffer, kFirstBytes);

    char* grown = static_cast<char*>(std::realloc(buffer, kSecondBytes));
    if (grown == nullptr) {
        std::free(buffer);
        return nullptr;
    }
    touch(grown, kSecondBytes);

    char* final = static_cast<char*>(std::realloc(grown, kFinalBytes));
    if (final == nullptr) {
        std::free(grown);
        return nullptr;
    }
    touch(final, kFinalBytes);
    return final;
}

/// The same growth, written the way C++ does it.
///
/// No realloc: allocate the larger block, copy, release the smaller one. Three
/// `new char[]` and two `delete[]`, leaving one live block -- the same shape as
/// the chain above, through a completely different pair of interceptors.
char* growChainCpp() {
    char* buffer = new char[kFirstBytes];
    touch(buffer, kFirstBytes);

    char* grown = new char[kSecondBytes];
    std::memcpy(grown, buffer, kFirstBytes);
    delete[] buffer;
    touch(grown, kSecondBytes);

    char* final = new char[kFinalBytes];
    std::memcpy(final, grown, kSecondBytes);
    delete[] grown;
    touch(final, kFinalBytes);

    return final;
}

/// The two realloc special cases, both of which must contribute nothing.
void reallocEdgeCases() {
    // realloc(NULL, n) is malloc(n).
    char* fromNull = static_cast<char*>(std::realloc(nullptr, 512));
    if (fromNull != nullptr) {
        touch(fromNull, 512);
        std::free(fromNull);
    }

    // Shrinking to a smaller size keeps one block, of the smaller size.
    char* shrinking = static_cast<char*>(std::malloc(4096));
    if (shrinking != nullptr) {
        touch(shrinking, 4096);
        char* smaller = static_cast<char*>(std::realloc(shrinking, 32));
        if (smaller != nullptr) {
            touch(smaller, 32);
            std::free(smaller);
        }
    }
}

}  // namespace

int main() {
    std::printf("realloc_chain starting\n");

    char* kept[kLeakedChains] = {};
    int keptCount = 0;

    for (int i = 0; i < kChains; ++i) {
        char* chain = growChain();
        if (chain == nullptr) {
            continue;
        }
        if (i < kFreedChains) {
            std::free(chain);
        } else {
            // LEAK #1: 40 chains, 1024 B each. The two intermediate sizes were
            // released by realloc itself and must not appear anywhere.
            kept[keptCount++] = chain;
        }
    }

    for (int i = 0; i < kCallocLeaks; ++i) {
        // LEAK #2: calloc, whose size is nmemb * size -- a multiplication a
        // tracker has to do, and can get wrong.
        void* zeroed = std::calloc(kCallocCount, kCallocSize);
        if (zeroed != nullptr) {
            touch(zeroed, kCallocCount * kCallocSize);
        }
    }

    char* keptCpp[kCppLeaked] = {};
    int keptCppCount = 0;
    for (int i = 0; i < kCppChains; ++i) {
        char* chain = growChainCpp();
        if (i < kCppChains - kCppLeaked) {
            delete[] chain;
        } else {
            // LEAK #3: the C++ spelling of the same mistake. The two
            // intermediate buffers were released explicitly and must not appear.
            keptCpp[keptCppCount++] = chain;
        }
    }

    reallocEdgeCases();

    // Read the kept pointers so they are unmistakably live at exit.
    for (int i = 0; i < keptCount; ++i) {
        g_sink = static_cast<unsigned char>(g_sink + static_cast<unsigned char>(kept[i][0]));
    }

    for (int i = 0; i < keptCppCount; ++i) {
        g_sink = static_cast<unsigned char>(g_sink + static_cast<unsigned char>(keptCpp[i][0]));
    }

    constexpr std::size_t expectedBytes =
        static_cast<std::size_t>(kLeakedChains) * kFinalBytes +
        static_cast<std::size_t>(kCallocLeaks) * kCallocCount * kCallocSize +
        static_cast<std::size_t>(kCppLeaked) * kFinalBytes;
    constexpr int expectedBlocks = kLeakedChains + kCallocLeaks + kCppLeaked;

    static_assert(expectedBytes == 64640, "docs/VALIDATION.md states 64,640 bytes");
    static_assert(expectedBlocks == 85, "docs/VALIDATION.md states 85 blocks");

    std::printf("  %d realloc chains grown, %d freed, %d kept\n", kChains, kFreedChains,
                keptCount);
    std::printf("  %d C++ grow chains, %d kept\n", kCppChains, keptCppCount);
    std::printf("  expected leak: %d blocks, %zu bytes\n", expectedBlocks, expectedBytes);
    std::printf("realloc_chain finished\n");
    return 0;
}
