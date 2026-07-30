/// @file PosixProcessRunner.hpp
/// @brief fork/exec implementation of IProcessRunner.

#pragma once

#include <cstddef>

#include "leakhunter/process/IProcessRunner.hpp"

namespace leakhunter::process {

/// Writes `LEAKHUNTER_PID=<pid>` into @p buffer, NUL-terminated and truncated
/// to @p capacity.
///
/// Exposed only so it can be unit-tested. It runs between fork() and exec(),
/// where snprintf is not guaranteed safe, so it is hand-rolled -- and a bug in
/// it would mean the agent silently mis-identifies which process it is in,
/// which is exactly the class of failure it exists to prevent.
void formatTracedPid(char* buffer, std::size_t capacity, long pid) noexcept;

/// Launches the target with fork(2) + execvp(3), injecting LD_PRELOAD and the
/// agent's configuration into the child environment.
///
/// stdout/stderr are inherited, so the monitored program behaves exactly as it
/// would without LeakHunter -- important when the target is interactive or is
/// itself part of a pipeline.
class PosixProcessRunner final : public IProcessRunner {
public:
    [[nodiscard]] Result<ProcessResult> run(const ProcessSpec& spec) override;
};

}  // namespace leakhunter::process
