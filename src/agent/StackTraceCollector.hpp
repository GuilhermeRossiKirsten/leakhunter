/// @file StackTraceCollector.hpp
/// @brief Captures raw program counters at the point of allocation.

#pragma once

#include <cstddef>
#include <cstdint>

namespace leakhunter::agent {

/// Walks the current call stack.
///
/// Backend is chosen at build time: libunwind when available (accurate even
/// without frame pointers), otherwise `_Unwind_Backtrace` from the compiler
/// runtime. Neither allocates on the fast path, which is the hard requirement
/// here -- this runs inside `malloc`.
///
/// Returned values are *call-site* addresses: each return address has 1
/// subtracted from it so that symbolisation attributes the frame to the call
/// instruction rather than to whatever follows it. This matters when a call is
/// the last instruction of a function or of a loop body.
///
/// @param out       destination buffer
/// @param capacity  size of @p out in entries
/// @param skip      innermost frames to drop (our own interceptors)
/// @return number of frames written
[[nodiscard]] std::size_t captureStack(std::uint64_t* out, std::size_t capacity,
                                       std::size_t skip) noexcept;

/// Name of the active backend, for the agent's verbose banner.
[[nodiscard]] const char* unwinderName() noexcept;

}  // namespace leakhunter::agent
