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

    /// Tells the reader that a missing end marker is expected.
    ///
    /// A trace normally ends with one; its absence means the target crashed, or
    /// called _exit(), and that is worth a warning. But when LeakHunter itself
    /// stopped a still-running service on the user's behalf, an unterminated
    /// trace is the *designed* outcome, and "the target did not shut down
    /// cleanly" would describe the user's own Ctrl-C as a malfunction.
    void expectNoEndMarker(bool expected) noexcept { endMarkerOptional_ = expected; }

private:
    std::filesystem::path path_;
    bool endMarkerOptional_ = false;
};

}  // namespace leakhunter::tracker
