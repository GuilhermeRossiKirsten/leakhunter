/// @file FileTraceSource.hpp
/// @brief Reads a binary trace file produced by the agent.

#pragma once

#include <filesystem>

#include "leakhunter/tracker/ITraceSource.hpp"

namespace leakhunter::tracker {

/// Streams records out of a trace file with a bounded read buffer, so a
/// multi-gigabyte trace never has to fit in memory.
///
/// Truncated traces are tolerated: whatever whole records exist are replayed
/// and `onTraceEnd` still fires (with `droppedRecords` flagged), because the
/// most interesting targets are the ones that crashed.
class FileTraceSource final : public ITraceSource {
public:
    explicit FileTraceSource(std::filesystem::path path);

    [[nodiscard]] Status replay(ITraceVisitor& visitor) override;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace leakhunter::tracker
