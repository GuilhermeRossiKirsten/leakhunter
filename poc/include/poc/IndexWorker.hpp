/// @file IndexWorker.hpp
/// @brief Background indexing across several threads.

#pragma once

#include <cstddef>

namespace poc {

/// Runs @p threadCount workers, each indexing @p tasksPerThread batches.
/// @return the number of batches indexed.
[[nodiscard]] std::size_t runIndexer(std::size_t threadCount, std::size_t tasksPerThread);

}  // namespace poc
