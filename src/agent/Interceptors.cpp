/// @file Interceptors.cpp
/// @brief The interposed allocator entry points.
///
/// LD_PRELOAD puts this object first in the dynamic symbol search order, so
/// every unresolved reference to `malloc`, `operator new` and friends in the
/// target -- and in every library it loads -- binds here instead of to libc or
/// libstdc++.
///
/// The shape of every hook is the same:
///
///   1. take a HookGuard (records only on the outermost entry per thread);
///   2. call the real implementation;
///   3. hand the result to the agent.
///
/// Step 1 is what makes this safe. Unwinding and trace writing can allocate,
/// and without the guard that allocation would recurse into this same hook
/// forever.
///
/// Declarations must match the ones in <stdlib.h>/<new>, including exception
/// specifications, or the compiler rejects the redefinition.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#include <dlfcn.h>
#include <unistd.h>

#include "Agent.hpp"
#include "BootstrapAllocator.hpp"
#include "RealFunctions.hpp"

#define LH_EXPORT __attribute__((visibility("default")))

namespace {

using leakhunter::agent::Agent;
using leakhunter::agent::HookGuard;
using leakhunter::ipc::AllocKind;
using leakhunter::ipc::FreeKind;
namespace bootstrap = leakhunter::agent::bootstrap;
namespace real = leakhunter::agent::real;

/// Shared body of every "allocate then record" hook.
[[nodiscard]] inline void* allocateAndRecord(void* pointer, std::size_t size,
                                             AllocKind kind) noexcept {
    if (pointer != nullptr) {
        Agent::instance().recordAllocation(pointer, size, kind);
    }
    return pointer;
}

/// Frees a pointer, taking care of the two special cases: blocks handed out by
/// the bootstrap arena (which must never reach the real allocator) and calls
/// arriving before the real `free` has been resolved.
inline void releasePointer(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    if (bootstrap::owns(pointer)) {
        return;  // arena memory is never reclaimed; see BootstrapAllocator.hpp
    }

    const real::FreeFn realFree = real::free();
    if (realFree == nullptr) {
        return;  // nothing sensible to do; leaking here is better than crashing
    }
    realFree(pointer);
}

}  // namespace

// ---------------------------------------------------------------------------
// C allocator
// ---------------------------------------------------------------------------

extern "C" LH_EXPORT void* malloc(std::size_t size) noexcept {
    const real::MallocFn realMalloc = real::malloc();
    if (realMalloc == nullptr) {
        return bootstrap::allocate(size);
    }

    void* pointer = realMalloc(size);

    const HookGuard guard;
    if (!guard.engaged()) {
        return pointer;
    }
    return allocateAndRecord(pointer, size, AllocKind::Malloc);
}

extern "C" LH_EXPORT void* calloc(std::size_t count, std::size_t size) noexcept {
    const real::CallocFn realCalloc = real::calloc();
    if (realCalloc == nullptr) {
        // This is the path dlsym() itself takes during start-up.
        return bootstrap::allocateZeroed(count, size);
    }

    void* pointer = realCalloc(count, size);

    const HookGuard guard;
    if (!guard.engaged()) {
        return pointer;
    }
    return allocateAndRecord(pointer, count * size, AllocKind::Calloc);
}

extern "C" LH_EXPORT void* realloc(void* pointer, std::size_t size) noexcept {
    if (bootstrap::owns(pointer)) {
        // Migrate out of the arena: allocate properly and copy what we can.
        void* fresh = ::malloc(size);
        if (fresh != nullptr) {
            const std::size_t previous = bootstrap::sizeOf(pointer);
            std::memcpy(fresh, pointer, previous < size ? previous : size);
        }
        return fresh;
    }

    const real::ReallocFn realRealloc = real::realloc();
    if (realRealloc == nullptr) {
        return bootstrap::allocate(size);
    }

    void* result = realRealloc(pointer, size);

    const HookGuard guard;
    if (!guard.engaged()) {
        return result;
    }

    // On failure realloc leaves the original block valid, so the free must not
    // be recorded -- doing so would turn a live block into a phantom leak later.
    if (pointer != nullptr && (result != nullptr || size == 0)) {
        Agent::instance().recordDeallocation(pointer, FreeKind::Realloc);
    }
    return allocateAndRecord(result, size, AllocKind::Realloc);
}

extern "C" LH_EXPORT void free(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }

    {
        const HookGuard guard;
        if (guard.engaged() && !bootstrap::owns(pointer)) {
            Agent::instance().recordDeallocation(pointer, FreeKind::Free);
        }
    }

    releasePointer(pointer);
}

extern "C" LH_EXPORT void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
    const real::AlignedAllocFn realAlignedAlloc = real::alignedAlloc();
    if (realAlignedAlloc == nullptr) {
        return bootstrap::allocate(size);
    }

    void* pointer = realAlignedAlloc(alignment, size);

    const HookGuard guard;
    if (!guard.engaged()) {
        return pointer;
    }
    return allocateAndRecord(pointer, size, AllocKind::AlignedAlloc);
}

// `out` is declared __nonnull by glibc, so the compiler rejects a null check
// on it. Callers passing null already have undefined behaviour before we see it.
extern "C" LH_EXPORT int posix_memalign(void** out, std::size_t alignment,
                                        std::size_t size) noexcept {
    const real::PosixMemalignFn realPosixMemalign = real::posixMemalign();
    if (realPosixMemalign == nullptr) {
        *out = bootstrap::allocate(size);
        return *out != nullptr ? 0 : ENOMEM;
    }

    const int status = realPosixMemalign(out, alignment, size);

    const HookGuard guard;
    if (!guard.engaged() || status != 0) {
        return status;
    }
    (void)allocateAndRecord(*out, size, AllocKind::AlignedAlloc);
    return status;
}

// ---------------------------------------------------------------------------
// Abrupt termination
//
// _exit() and _Exit() go straight to the kernel: no atexit handlers, no library
// destructors, so the agent's shutdown never runs and the buffered trace would
// be lost. Interposing them costs one flush on a path that is about to end the
// process anyway.
// ---------------------------------------------------------------------------

namespace {

using ExitFn = void (*)(int);

[[noreturn]] void flushAndExit(const char* symbol, int status) {
    Agent::instance().emergencyFlush();

    // NOLINTNEXTLINE(google-readability-casting) -- dlsym returns void*
    const auto realExit = reinterpret_cast<ExitFn>(::dlsym(RTLD_NEXT, symbol));
    if (realExit != nullptr) {
        realExit(status);
    }
    ::_Exit(status);  // unreachable in practice; keeps the noreturn contract
}

}  // namespace

extern "C" LH_EXPORT void _exit(int status) { flushAndExit("_exit", status); }
extern "C" LH_EXPORT void _Exit(int status) { flushAndExit("_Exit", status); }

// ---------------------------------------------------------------------------
// C++ allocator
//
// These bypass our own `malloc` hook and call the real allocator directly, so
// a `new` is recorded exactly once -- as a `new`, not as a `malloc`.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] void* newImpl(std::size_t size, AllocKind kind, bool throwOnFailure) {
    const real::MallocFn realMalloc = real::malloc();

    // operator new(0) must return a distinct, freeable pointer.
    void* pointer =
        realMalloc != nullptr ? realMalloc(size != 0 ? size : 1) : bootstrap::allocate(size);

    if (pointer == nullptr) {
        if (throwOnFailure) {
            throw std::bad_alloc();
        }
        return nullptr;
    }

    const HookGuard guard;
    if (guard.engaged()) {
        Agent::instance().recordAllocation(pointer, size, kind);
    }
    return pointer;
}

/// @param kind Delete or DeleteArray -- recorded so the host can pair it with
///             the AllocKind of the same address and spot `new[]`/`delete`.
void deleteImpl(void* pointer, FreeKind kind) noexcept {
    if (pointer == nullptr) {
        return;
    }
    {
        const HookGuard guard;
        if (guard.engaged() && !bootstrap::owns(pointer)) {
            Agent::instance().recordDeallocation(pointer, kind);
        }
    }
    releasePointer(pointer);
}

}  // namespace

LH_EXPORT void* operator new(std::size_t size) { return newImpl(size, AllocKind::New, true); }

LH_EXPORT void* operator new[](std::size_t size) {
    return newImpl(size, AllocKind::NewArray, true);
}

LH_EXPORT void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return newImpl(size, AllocKind::New, false);
}

LH_EXPORT void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return newImpl(size, AllocKind::NewArray, false);
}

LH_EXPORT void operator delete(void* pointer) noexcept { deleteImpl(pointer, FreeKind::Delete); }

LH_EXPORT void operator delete[](void* pointer) noexcept {
    deleteImpl(pointer, FreeKind::DeleteArray);
}

LH_EXPORT void operator delete(void* pointer, std::size_t) noexcept {
    deleteImpl(pointer, FreeKind::Delete);
}

LH_EXPORT void operator delete[](void* pointer, std::size_t) noexcept {
    deleteImpl(pointer, FreeKind::DeleteArray);
}

LH_EXPORT void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    deleteImpl(pointer, FreeKind::Delete);
}

LH_EXPORT void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    deleteImpl(pointer, FreeKind::DeleteArray);
}

// --- aligned forms (C++17) -------------------------------------------------

namespace {

[[nodiscard]] void* alignedNewImpl(std::size_t size, std::align_val_t alignment, AllocKind kind,
                                   bool throwOnFailure) {
    const auto rawAlignment = static_cast<std::size_t>(alignment);
    const real::AlignedAllocFn realAlignedAlloc = real::alignedAlloc();

    // aligned_alloc requires the size to be a multiple of the alignment.
    const std::size_t rounded =
        rawAlignment != 0 ? ((size + rawAlignment - 1) / rawAlignment) * rawAlignment : size;

    void* pointer = realAlignedAlloc != nullptr ? realAlignedAlloc(rawAlignment, rounded)
                                                : bootstrap::allocate(rounded);

    if (pointer == nullptr) {
        if (throwOnFailure) {
            throw std::bad_alloc();
        }
        return nullptr;
    }

    const HookGuard guard;
    if (guard.engaged()) {
        Agent::instance().recordAllocation(pointer, size, kind);
    }
    return pointer;
}

}  // namespace

LH_EXPORT void* operator new(std::size_t size, std::align_val_t alignment) {
    return alignedNewImpl(size, alignment, AllocKind::New, true);
}

LH_EXPORT void* operator new[](std::size_t size, std::align_val_t alignment) {
    return alignedNewImpl(size, alignment, AllocKind::NewArray, true);
}

LH_EXPORT void* operator new(std::size_t size, std::align_val_t alignment,
                             const std::nothrow_t&) noexcept {
    return alignedNewImpl(size, alignment, AllocKind::New, false);
}

LH_EXPORT void* operator new[](std::size_t size, std::align_val_t alignment,
                               const std::nothrow_t&) noexcept {
    return alignedNewImpl(size, alignment, AllocKind::NewArray, false);
}

LH_EXPORT void operator delete(void* pointer, std::align_val_t) noexcept {
    deleteImpl(pointer, FreeKind::Delete);
}

LH_EXPORT void operator delete[](void* pointer, std::align_val_t) noexcept {
    deleteImpl(pointer, FreeKind::DeleteArray);
}

LH_EXPORT void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    deleteImpl(pointer, FreeKind::Delete);
}

LH_EXPORT void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    deleteImpl(pointer, FreeKind::DeleteArray);
}

LH_EXPORT void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    deleteImpl(pointer, FreeKind::Delete);
}

LH_EXPORT void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    deleteImpl(pointer, FreeKind::DeleteArray);
}
