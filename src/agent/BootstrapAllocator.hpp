/// @file BootstrapAllocator.hpp
/// @brief Static arena used before the real allocator can be resolved.
///
/// Resolving `malloc` requires dlsym(), and glibc's dlsym() allocates. That is
/// a chicken-and-egg problem: the first call into our `calloc` interceptor
/// happens while we are still trying to find the real `calloc`.
///
/// The classic answer is a small bump allocator carved out of BSS. Allocations
/// served from it are never reused and never returned to the system; `free()`
/// recognises them by address range and ignores them. A few kilobytes is all
/// the loader ever needs.

#pragma once

#include <cstddef>

namespace leakhunter::agent::bootstrap {

/// Fixed capacity of the arena. Sized generously: exhausting it aborts start-up.
inline constexpr std::size_t kCapacity = 128U * 1024U;

/// @return an aligned block, or nullptr when the arena is exhausted.
[[nodiscard]] void* allocate(std::size_t size) noexcept;

/// allocate() + zero-fill. The arena starts zeroed and is never reused, so this
/// is normally free.
[[nodiscard]] void* allocateZeroed(std::size_t count, std::size_t size) noexcept;

/// True when @p pointer was handed out by this arena and must not reach free().
[[nodiscard]] bool owns(const void* pointer) noexcept;

/// Usable size of a block from this arena; 0 when it is not ours.
[[nodiscard]] std::size_t sizeOf(const void* pointer) noexcept;

/// Bytes handed out so far -- diagnostics only.
[[nodiscard]] std::size_t used() noexcept;

}  // namespace leakhunter::agent::bootstrap
