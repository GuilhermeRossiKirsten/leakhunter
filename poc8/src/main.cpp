/// @file poc8/src/main.cpp
/// @brief Leaks on the path an exception takes.
///
/// The most common real leak in C++ is not a forgotten `delete` at the end of a
/// function -- it is a `delete` that is never reached because something threw
/// on the way there. The happy path is tested; the throwing path is the one
/// that leaks in production.
///
/// This program does both, in equal numbers, so the report has to separate
/// them: 50 raw allocations abandoned by a throw, and 50 identical allocations
/// held by unique_ptr that the same throw releases correctly. Reporting 100
/// would mean RAII is not understood; reporting 0 would mean the throw path is
/// not seen at all. See docs/VALIDATION.md.

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

// --- the arithmetic --------------------------------------------------------
//
//   raw, abandoned by throw : 50 x 200 B = 10,000 B   <- LEAK
//   unique_ptr, same throw  : 50 x 200 B =      0 B   <- released while unwinding
//   nested, two per throw   : 20 x (128 + 64) = 3,840 B  <- LEAK
//   --------------------------------------------------------------------------
//   TOTAL LEAKED            : 90 blocks   = 13,840 B

constexpr int kThrows = 50;
constexpr std::size_t kBufferBytes = 200;

constexpr int kNestedThrows = 20;
constexpr std::size_t kOuterBytes = 128;
constexpr std::size_t kInnerBytes = 64;

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
    raw[0] = 0x11;
    raw[bytes - 1] = 0x22;
    g_sink = static_cast<unsigned char>(g_sink + raw[0] + raw[bytes - 1]);
}

/// Allocates, then throws before the delete. Textbook, and still everywhere.
void parseRecordUnsafe(int id) {
    char* scratch = new char[kBufferBytes];
    touch(scratch, kBufferBytes);

    if (id % 2 == 0) {
        // LEAK #1: the throw skips the delete below. Nothing owns `scratch`,
        // so unwinding cannot help it.
        throw std::runtime_error("malformed record");
    }

    delete[] scratch;
}

/// The same function, with the buffer owned. The throw is identical; the
/// outcome is not.
void parseRecordSafe(int id) {
    auto scratch = std::make_unique<char[]>(kBufferBytes);
    touch(scratch.get(), kBufferBytes);

    if (id % 2 == 0) {
        // Unwinding runs ~unique_ptr, which releases the buffer. This must not
        // appear in the report.
        throw std::runtime_error("malformed record");
    }
}

/// Two allocations, an exception between them: the first is abandoned, and so
/// is the second, because nothing links them to a destructor.
void buildNested() {
    char* outer = new char[kOuterBytes];
    touch(outer, kOuterBytes);

    char* inner = new char[kInnerBytes];
    touch(inner, kInnerBytes);

    // LEAK #2: both blocks are lost. Note that a reachability-based tool would
    // call these two *separate* direct leaks, exactly as this one does --
    // neither points at the other.
    throw std::logic_error("nested failure");
}

}  // namespace

int main() {
    std::printf("exception_paths starting\n");

    int thrown = 0;
    int leakedRaw = 0;

    // `id * 2` makes every call take the throwing branch, so the count is
    // exactly kThrows rather than "about half".
    for (int id = 0; id < kThrows; ++id) {
        try {
            parseRecordUnsafe(id * 2);
        } catch (const std::exception&) {
            ++thrown;
            ++leakedRaw;
        }
    }

    for (int id = 0; id < kThrows; ++id) {
        try {
            parseRecordSafe(id * 2);
        } catch (const std::exception&) {
            ++thrown;
        }
    }

    int leakedNested = 0;
    for (int i = 0; i < kNestedThrows; ++i) {
        try {
            buildNested();
        } catch (const std::exception&) {
            leakedNested += 2;  // both blocks
        }
    }

    constexpr std::size_t expectedBytes =
        static_cast<std::size_t>(kThrows) * kBufferBytes +
        static_cast<std::size_t>(kNestedThrows) * (kOuterBytes + kInnerBytes);
    constexpr int expectedBlocks = kThrows + kNestedThrows * 2;

    static_assert(expectedBytes == 13840, "docs/VALIDATION.md states 13,840 bytes");
    static_assert(expectedBlocks == 90, "docs/VALIDATION.md states 90 blocks");

    std::printf("  %d exceptions thrown and caught\n", thrown);
    std::printf("  %d raw buffers abandoned, %d nested blocks abandoned\n", leakedRaw,
                leakedNested);
    std::printf("  %d unique_ptr buffers released while unwinding (must not be reported)\n",
                kThrows);
    std::printf("  expected leak: %d blocks, %zu bytes\n", expectedBlocks, expectedBytes);
    std::printf("exception_paths finished\n");
    return 0;
}
