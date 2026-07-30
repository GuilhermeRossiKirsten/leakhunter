// execvpe() and strsignal() are GNU/POSIX extensions; request them explicitly
// instead of relying on the compiler predefining _GNU_SOURCE.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "leakhunter/process/PosixProcessRunner.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "leakhunter/core/Logger.hpp"
#include "leakhunter/ipc/TraceFormat.hpp"
#include "leakhunter/process/PosixProcessRunner.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define LEAKHUNTER_HAS_FORK 1
extern "C" char** environ;  // NOLINT -- POSIX global
#else
#define LEAKHUNTER_HAS_FORK 0
#endif

namespace leakhunter::process {

// Defined outside the fork-only block so the unit tests link on any platform.
void formatTracedPid(char* buffer, std::size_t capacity, long pid) noexcept {
    if (buffer == nullptr || capacity == 0) {
        return;
    }

    const std::string_view prefix{ipc::kEnvTracedPid};
    std::size_t written = 0;

    // Every branch below leaves room for the terminator.
    for (const char c : prefix) {
        if (written + 1 >= capacity) {
            break;
        }
        buffer[written++] = c;
    }
    if (written + 1 < capacity) {
        buffer[written++] = '=';
    }

    // A negative pid cannot reach here from fork(), but clamping costs nothing
    // and the alternative is a '-' that the agent's strtol would misread.
    if (pid < 0) {
        pid = 0;
    }

    // Digits are produced least-significant first, then emitted in reverse.
    char digits[24];
    std::size_t digitCount = 0;
    do {
        digits[digitCount++] = static_cast<char>('0' + (pid % 10));
        pid /= 10;
    } while (pid > 0 && digitCount < sizeof(digits));

    while (digitCount > 0 && written + 1 < capacity) {
        buffer[written++] = digits[--digitCount];
    }
    buffer[written] = '\0';
}

#if LEAKHUNTER_HAS_FORK

namespace {

/// Appends to an existing LD_PRELOAD rather than replacing it: the target may
/// legitimately rely on its own preloads, and clobbering them would change the
/// behaviour we are supposed to be observing.
[[nodiscard]] std::string buildPreloadValue(const std::filesystem::path& library) {
    const char* existing = std::getenv("LD_PRELOAD");
    if (existing == nullptr || *existing == '\0') {
        return library.string();
    }
    return fmt::format("{}:{}", library.string(), existing);
}

/// Builds a NUL-terminated char* array; the backing strings must outlive it.
[[nodiscard]] std::vector<char*> toRawPointers(std::vector<std::string>& storage) {
    std::vector<char*> raw;
    raw.reserve(storage.size() + 1);
    for (std::string& item : storage) {
        raw.push_back(item.data());
    }
    raw.push_back(nullptr);
    return raw;
}

[[nodiscard]] std::vector<std::string> buildEnvironment(const ProcessSpec& spec) {
    std::vector<std::string> result;
    std::map<std::string, std::string> overrides = spec.environment;

    if (!spec.preloadLibrary.empty()) {
        overrides["LD_PRELOAD"] = buildPreloadValue(spec.preloadLibrary);
    }

    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        const std::size_t equals = text.find('=');
        const std::string key{equals == std::string_view::npos ? text : text.substr(0, equals)};
        if (overrides.find(key) == overrides.end()) {
            result.emplace_back(text);
        }
    }

    for (const auto& [key, value] : overrides) {
        result.push_back(fmt::format("{}={}", key, value));
    }
    return result;
}

/// Replaces the child image. Only returns on failure, in which case errno is
/// reported to the parent through @p failureFd -- that write is the only way
/// the parent can tell "the program does not exist" from "the program ran and
/// exited immediately".
///
/// @p pidSlot is the index in @p envp reserved for LEAKHUNTER_PID. It can only
/// be filled in here, after fork(), because that is the first moment the traced
/// process's own pid exists.
[[noreturn]] void execInChild(std::vector<char*>& argv, std::vector<char*>& envp,
                              std::size_t pidSlot, int failureFd) {
    // Lives until execvpe() replaces the image, which is all that is required.
    char pidVariable[64];
    formatTracedPid(pidVariable, sizeof(pidVariable), static_cast<long>(::getpid()));
    envp[pidSlot] = pidVariable;

#if defined(__linux__)
    ::execvpe(argv[0], argv.data(), envp.data());
#else
    // execvpe() is Linux-only; elsewhere swap the global environment first.
    environ = envp.data();
    ::execvp(argv[0], argv.data());
#endif

    const int failure = errno;
    [[maybe_unused]] const ssize_t ignored = ::write(failureFd, &failure, sizeof(failure));
    ::_exit(127);
}

}  // namespace

Result<ProcessResult> PosixProcessRunner::run(const ProcessSpec& spec) {
    if (spec.command.empty()) {
        return Error{"no command to run"};
    }

    std::vector<std::string> argvStorage = spec.command;
    std::vector<std::string> envStorage = buildEnvironment(spec);

    // Reserve the LEAKHUNTER_PID slot before forking, so the child only has to
    // store a pointer -- no allocation on the far side of fork().
    envStorage.emplace_back("LEAKHUNTER_PID=0");

    std::vector<char*> argv = toRawPointers(argvStorage);
    std::vector<char*> envp = toRawPointers(envStorage);
    const std::size_t pidSlot = envStorage.size() - 1;

    // A close-on-exec pipe reports exec failures: if exec succeeds the pipe is
    // closed and the parent reads EOF; otherwise the child writes errno into
    // it. Without this, a missing binary would look like a program that started
    // and exited immediately.
    int execPipe[2] = {-1, -1};
    if (::pipe(execPipe) != 0) {
        return Error{fmt::format("pipe() failed: {}", std::strerror(errno))};
    }
    ::fcntl(execPipe[1], F_SETFD, FD_CLOEXEC);

    const auto startedAt = std::chrono::steady_clock::now();

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int savedErrno = errno;
        ::close(execPipe[0]);
        ::close(execPipe[1]);
        return Error{fmt::format("fork() failed: {}", std::strerror(savedErrno))};
    }

    if (pid == 0) {
        // --- child ---------------------------------------------------------
        ::close(execPipe[0]);

        if (!spec.workingDirectory.empty() && ::chdir(spec.workingDirectory.c_str()) != 0) {
            const int err = errno;
            [[maybe_unused]] const ssize_t ignored = ::write(execPipe[1], &err, sizeof(err));
            ::_exit(127);
        }

        execInChild(argv, envp, pidSlot, execPipe[1]);
    }

    // --- parent ------------------------------------------------------------
    ::close(execPipe[1]);

    int childErrno = 0;
    ssize_t bytesRead = 0;
    do {
        bytesRead = ::read(execPipe[0], &childErrno, sizeof(childErrno));
    } while (bytesRead < 0 && errno == EINTR);
    ::close(execPipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return Error{fmt::format("waitpid() failed: {}", std::strerror(errno))};
        }
    }

    if (bytesRead == static_cast<ssize_t>(sizeof(childErrno))) {
        return Error{fmt::format("could not execute '{}': {}", spec.command.front(),
                                 std::strerror(childErrno))};
    }

    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

    ProcessResult result;
    result.started = true;
    result.pid = static_cast<std::uint64_t>(pid);
    result.durationMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.terminatingSignal = WTERMSIG(status);
        result.exitCode = 128 + result.terminatingSignal;
        log::warn("target terminated by signal {} ({})", result.terminatingSignal,
                  ::strsignal(result.terminatingSignal));
    }

    return result;
}

#else  // !LEAKHUNTER_HAS_FORK

Result<ProcessResult> PosixProcessRunner::run(const ProcessSpec&) {
    return Error{
        "process launching is only implemented for POSIX platforms; "
        "see docs/ROADMAP.md for Windows support"};
}

#endif

}  // namespace leakhunter::process
