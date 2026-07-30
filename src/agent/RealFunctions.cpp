#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "RealFunctions.hpp"

#include <atomic>

#include <dlfcn.h>

namespace leakhunter::agent::real {
namespace {

/// True while this thread is inside dlsym(). Guards against the recursion that
/// would otherwise occur when dlsym() allocates through the very symbol it is
/// being asked to resolve. `initial-exec` avoids a __tls_get_addr call, which
/// could itself allocate on the first access.
__attribute__((tls_model("initial-exec"))) thread_local bool t_resolving = false;

/// Resolves @p name in the next object after ours in the search order, i.e. the
/// implementation we are shadowing.
template <typename Fn>
[[nodiscard]] Fn resolve(std::atomic<void*>& cache, const char* name) noexcept {
    if (void* cached = cache.load(std::memory_order_acquire); cached != nullptr) {
        return reinterpret_cast<Fn>(cached);
    }
    if (t_resolving) {
        return nullptr;  // re-entered from inside dlsym(); caller uses bootstrap
    }

    t_resolving = true;
    void* resolved = ::dlsym(RTLD_NEXT, name);
    t_resolving = false;

    if (resolved == nullptr) {
        return nullptr;
    }

    // Benign race: two threads may resolve concurrently and store the same
    // address, so a plain store is enough.
    cache.store(resolved, std::memory_order_release);
    return reinterpret_cast<Fn>(resolved);
}

std::atomic<void*> g_malloc{nullptr};
std::atomic<void*> g_calloc{nullptr};
std::atomic<void*> g_realloc{nullptr};
std::atomic<void*> g_free{nullptr};
std::atomic<void*> g_alignedAlloc{nullptr};
std::atomic<void*> g_posixMemalign{nullptr};

}  // namespace

MallocFn malloc() noexcept { return resolve<MallocFn>(g_malloc, "malloc"); }
CallocFn calloc() noexcept { return resolve<CallocFn>(g_calloc, "calloc"); }
ReallocFn realloc() noexcept { return resolve<ReallocFn>(g_realloc, "realloc"); }
FreeFn free() noexcept { return resolve<FreeFn>(g_free, "free"); }

AlignedAllocFn alignedAlloc() noexcept {
    return resolve<AlignedAllocFn>(g_alignedAlloc, "aligned_alloc");
}

PosixMemalignFn posixMemalign() noexcept {
    return resolve<PosixMemalignFn>(g_posixMemalign, "posix_memalign");
}

void resolveAll() noexcept {
    (void)malloc();
    (void)calloc();
    (void)realloc();
    (void)free();
    (void)alignedAlloc();
    (void)posixMemalign();
}

}  // namespace leakhunter::agent::real
