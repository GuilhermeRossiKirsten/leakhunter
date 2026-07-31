/// @file poc10/src/main.cpp
/// @brief Ownership that looks safe and is not.
///
/// Three ways to leak while holding a smart pointer:
///
///   1. a `shared_ptr` cycle -- reference counts that never reach zero;
///   2. `unique_ptr::release()`, which hands ownership back and is easy to
///      call by reflex instead of `reset()`;
///   3. a C API whose `create` has no matching `destroy` on one path.
///
/// The same file also builds the `weak_ptr` version of the cycle, which must
/// come back clean. Both halves are identical apart from one word.
///
/// **On exactness.** The raw buffers below are sized by constants, so their
/// bytes are arithmetic. The `shared_ptr` control blocks are not: their size is
/// a property of the standard library, not of this program. The *block count*
/// is still exact, so docs/VALIDATION.md checks blocks exactly and splits the
/// byte total into the part that is arithmetic and the part that is measured.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

// --- the arithmetic --------------------------------------------------------
//
//   shared_ptr cycle : 25 pairs -> 50 nodes
//       payloads     : 50 x 256 B = 12,800 B   (exact)
//       node blocks  : 50         = ? B        (make_shared, library-defined)
//   unique_ptr::release()         : 30 x 512 B = 15,360 B   (exact)
//   C handle never destroyed      : 20 x (32 + 128) = 3,200 B  (exact)
//   --------------------------------------------------------------------------
//   TOTAL LEAKED : 150 blocks; 31,360 B exact + 50 library-sized blocks
//
//   weak_ptr cycle : 25 pairs -> must leak nothing at all

constexpr int kCycles = 25;
constexpr std::size_t kPayloadBytes = 256;

constexpr int kReleased = 30;
constexpr std::size_t kReleasedBytes = 512;

constexpr int kLeakedHandles = 20;
constexpr int kClosedHandles = 40;  // the same API, used correctly
constexpr std::size_t kHandleBufferBytes = 128;

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
    raw[0] = 0x77;
    raw[bytes - 1] = 0x88;
    g_sink = static_cast<unsigned char>(g_sink + raw[0] + raw[bytes - 1]);
}

// --- 1. the cycle ----------------------------------------------------------

struct Node {
    char* payload = nullptr;
    std::shared_ptr<Node> peer;  // strong both ways: the counts never hit zero

    ~Node() { delete[] payload; }
};

struct SafeNode {
    char* payload = nullptr;
    std::weak_ptr<SafeNode> peer;  // the one word that changes everything

    ~SafeNode() { delete[] payload; }
};

/// Two nodes pointing at each other. Both refcounts stay at 1 after the local
/// shared_ptrs die, so neither destructor runs and neither payload is freed.
void buildCycle() {
    auto first = std::make_shared<Node>();
    auto second = std::make_shared<Node>();

    first->payload = new char[kPayloadBytes];
    second->payload = new char[kPayloadBytes];
    touch(first->payload, kPayloadBytes);
    touch(second->payload, kPayloadBytes);

    // LEAK #1: the cycle. shared_ptr is not a garbage collector, and this is
    // the case people are most surprised by.
    first->peer = second;
    second->peer = first;
}

/// The same graph with a weak back-reference. Everything is released.
void buildSafeCycle() {
    auto first = std::make_shared<SafeNode>();
    auto second = std::make_shared<SafeNode>();

    first->payload = new char[kPayloadBytes];
    second->payload = new char[kPayloadBytes];
    touch(first->payload, kPayloadBytes);
    touch(second->payload, kPayloadBytes);

    first->peer = second;
    second->peer = first;  // weak: does not keep `first` alive
}

// --- 2. release() ----------------------------------------------------------

/// Hands the pointer out of the unique_ptr and drops it on the floor.
///
/// `release()` returns the raw pointer *and gives up ownership*. Called where
/// `reset()` or `get()` was meant, it turns a correct program into a leaking
/// one with no visible change at the call site.
void releaseOwnership() {
    auto buffer = std::make_unique<char[]>(kReleasedBytes);
    touch(buffer.get(), kReleasedBytes);

    // LEAK #2: nothing owns this now.
    char* raw = buffer.release();
    g_sink = static_cast<unsigned char>(g_sink + static_cast<unsigned char>(raw[0]));
}

// --- 3. the C boundary -----------------------------------------------------

struct Handle {
    char* buffer;
    std::size_t bytes;
    std::size_t generation;
    void* reserved;
};

static_assert(sizeof(Handle) == 32, "the document's arithmetic assumes a 32-byte handle");

Handle* handleCreate() {
    auto* handle = static_cast<Handle*>(std::malloc(sizeof(Handle)));
    if (handle == nullptr) {
        return nullptr;
    }
    handle->buffer = static_cast<char*>(std::malloc(kHandleBufferBytes));
    handle->bytes = kHandleBufferBytes;
    handle->generation = 1;
    handle->reserved = nullptr;
    if (handle->buffer != nullptr) {
        touch(handle->buffer, kHandleBufferBytes);
    }
    return handle;
}

void handleDestroy(Handle* handle) {
    if (handle == nullptr) {
        return;
    }
    std::free(handle->buffer);
    std::free(handle);
}

}  // namespace

int main() {
    std::printf("ownership_zoo starting\n");

    for (int i = 0; i < kCycles; ++i) {
        buildCycle();
        buildSafeCycle();  // identical, minus one word -- must stay clean
    }

    for (int i = 0; i < kReleased; ++i) {
        releaseOwnership();
    }

    for (int i = 0; i < kClosedHandles; ++i) {
        Handle* handle = handleCreate();
        handleDestroy(handle);  // used correctly
    }

    for (int i = 0; i < kLeakedHandles; ++i) {
        // LEAK #3: two blocks each -- the handle and the buffer it owns. A
        // reachability-based tool calls the buffer an *indirect* leak, since it
        // is only unreachable because the handle was lost. This tool reports
        // both as leaks, which is the documented difference.
        Handle* handle = handleCreate();
        if (handle != nullptr) {
            g_sink = static_cast<unsigned char>(g_sink + static_cast<unsigned char>(handle->buffer[0]));
        }
    }

    constexpr std::size_t exactBytes =
        static_cast<std::size_t>(kCycles) * 2 * kPayloadBytes +
        static_cast<std::size_t>(kReleased) * kReleasedBytes +
        static_cast<std::size_t>(kLeakedHandles) * (sizeof(Handle) + kHandleBufferBytes);
    constexpr int exactBlocks = kCycles * 2 + kReleased + kLeakedHandles * 2;
    constexpr int libraryBlocks = kCycles * 2;  // make_shared, size library-defined

    static_assert(exactBytes == 31360, "docs/VALIDATION.md states 31,360 exact bytes");
    static_assert(exactBlocks == 120, "docs/VALIDATION.md states 120 exactly-sized blocks");
    static_assert(libraryBlocks == 50, "docs/VALIDATION.md states 50 library-sized blocks");

    std::printf("  %d shared_ptr cycles leaked, %d weak_ptr cycles released\n", kCycles, kCycles);
    std::printf("  %d unique_ptr buffers released out of ownership\n", kReleased);
    std::printf("  %d handles destroyed, %d abandoned\n", kClosedHandles, kLeakedHandles);
    std::printf("  expected leak: %d blocks (%d exact + %d library-sized), %zu exact bytes\n",
                exactBlocks + libraryBlocks, exactBlocks, libraryBlocks, exactBytes);
    std::printf("ownership_zoo finished\n");
    return 0;
}
