/// @file ReportBuilder.hpp
/// @brief Summarises cached documents.

#pragma once

#include <cstddef>
#include <string>

#include "poc/DocumentCache.hpp"

namespace poc {

/// Reads @p count documents out of @p cache and returns a one-line summary.
///
/// Separate translation unit from DocumentCache on purpose: the allocation
/// happens there, the release happens here, and no compiler can see both.
[[nodiscard]] std::string summarise(const DocumentCache& cache, long firstId, std::size_t count);

}  // namespace poc
