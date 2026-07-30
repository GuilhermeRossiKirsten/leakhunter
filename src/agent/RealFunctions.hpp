/// @file RealFunctions.hpp
/// @brief Lazy dlsym(RTLD_NEXT, ...) lookups of the allocator we shadow.

#pragma once

#include <cstddef>

namespace leakhunter::agent::real {

using MallocFn = void* (*)(std::size_t);
using CallocFn = void* (*)(std::size_t, std::size_t);
using ReallocFn = void* (*)(void*, std::size_t);
using FreeFn = void (*)(void*);
using AlignedAllocFn = void* (*)(std::size_t, std::size_t);
using PosixMemalignFn = int (*)(void**, std::size_t, std::size_t);

/// Each accessor resolves its symbol once and caches it.
///
/// They may return nullptr, and callers must handle that: dlsym() itself
/// allocates, so the very first call re-enters our interceptors. While a
/// lookup is in flight the accessors report "not available yet" and the
/// interceptor falls back to the bootstrap arena.
[[nodiscard]] MallocFn malloc() noexcept;
[[nodiscard]] CallocFn calloc() noexcept;
[[nodiscard]] ReallocFn realloc() noexcept;
[[nodiscard]] FreeFn free() noexcept;
[[nodiscard]] AlignedAllocFn alignedAlloc() noexcept;
[[nodiscard]] PosixMemalignFn posixMemalign() noexcept;

/// Resolves everything up front. Called from the agent constructor, where
/// re-entrancy is harmless because tracing is not enabled yet.
void resolveAll() noexcept;

}  // namespace leakhunter::agent::real
