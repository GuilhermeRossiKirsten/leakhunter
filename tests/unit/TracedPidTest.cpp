/// The LEAKHUNTER_PID environment string.
///
/// This is hand-rolled because it runs between fork() and exec(), where
/// snprintf is not guaranteed safe. It is tested because getting it wrong makes
/// the agent mis-identify which process it is in -- and that failure mode is a
/// report full of a child process's leaks, presented as the target's.

#include <cstring>
#include <string>

#include "TestFramework.hpp"
#include "leakhunter/ipc/TraceFormat.hpp"
#include "leakhunter/process/PosixProcessRunner.hpp"

using leakhunter::process::formatTracedPid;

namespace {

std::string format(long pid, std::size_t capacity = 64) {
    // Poison the buffer so a missing terminator shows up as garbage rather than
    // as an accidentally-correct empty string.
    char buffer[128];
    std::memset(buffer, 'X', sizeof(buffer));
    formatTracedPid(buffer, capacity, pid);
    return std::string{buffer};
}

}  // namespace

LH_TEST(TracedPid, produces_the_variable_the_agent_reads) {
    LH_CHECK_EQ(format(1234), std::string{"LEAKHUNTER_PID=1234"});

    // The name has to match the constant the agent looks up, or the whole
    // mechanism silently does nothing.
    const std::string expectedPrefix = std::string{leakhunter::ipc::kEnvTracedPid} + "=";
    LH_CHECK(format(1234).rfind(expectedPrefix, 0) == 0);
}

LH_TEST(TracedPid, handles_the_range_of_real_pids) {
    LH_CHECK_EQ(format(1), std::string{"LEAKHUNTER_PID=1"});
    LH_CHECK_EQ(format(7), std::string{"LEAKHUNTER_PID=7"});
    LH_CHECK_EQ(format(99999), std::string{"LEAKHUNTER_PID=99999"});
    // Linux allows pid_max up to 2^22; 64-bit hosts can go higher still.
    LH_CHECK_EQ(format(4194304), std::string{"LEAKHUNTER_PID=4194304"});
    LH_CHECK_EQ(format(9223372036854775807L),
                std::string{"LEAKHUNTER_PID=9223372036854775807"});
}

LH_TEST(TracedPid, zero_is_written_not_skipped) {
    // The reserved slot starts as "LEAKHUNTER_PID=0"; a formatter that emitted
    // nothing for 0 would leave a variable the agent would parse as "absent".
    LH_CHECK_EQ(format(0), std::string{"LEAKHUNTER_PID=0"});
}

LH_TEST(TracedPid, a_negative_pid_is_clamped_rather_than_signed) {
    // fork() never yields one, but a '-' would make the agent's strtol read a
    // negative number and take the "no expectation" branch.
    LH_CHECK_EQ(format(-5), std::string{"LEAKHUNTER_PID=0"});
}

LH_TEST(TracedPid, a_short_buffer_truncates_and_still_terminates) {
    // Truncation must never run past the end or leave the string unterminated.
    for (std::size_t capacity = 1; capacity <= 24; ++capacity) {
        const std::string result = format(1234, capacity);
        LH_CHECK(result.size() < capacity);

        // Whatever fits must be a prefix of the correct answer.
        const std::string full = "LEAKHUNTER_PID=1234";
        LH_CHECK(full.rfind(result, 0) == 0);
    }
}

LH_TEST(TracedPid, a_zero_capacity_buffer_is_left_alone) {
    char buffer[8];
    std::memset(buffer, 'X', sizeof(buffer));
    formatTracedPid(buffer, 0, 1234);
    LH_CHECK_EQ(buffer[0], 'X');
}

LH_TEST(TracedPid, a_null_buffer_is_not_dereferenced) {
    formatTracedPid(nullptr, 64, 1234);  // must simply return
    LH_CHECK(true);
}

LH_TEST(TracedPid, the_exact_fit_capacity_works) {
    // "LEAKHUNTER_PID=1234" is 19 characters, so 20 bytes is the exact fit.
    LH_CHECK_EQ(format(1234, 20), std::string{"LEAKHUNTER_PID=1234"});
    LH_CHECK_EQ(format(1234, 19), std::string{"LEAKHUNTER_PID=123"});
}
