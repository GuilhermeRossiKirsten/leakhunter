/// A target that fork+exec's a child which also allocates.
///
/// This is the shape of every build tool, test harness and wrapper script, and
/// it used to break LeakHunter badly. The child inherits LD_PRELOAD and
/// LEAKHUNTER_TRACE, so its agent would open the *same* trace path with
/// O_TRUNC and destroy everything the parent had already flushed. The host then
/// read the child's header, the child's records and the child's end marker, and
/// reported all of it as the target's -- with droppedRecords still zero.
///
/// The parent here allocates enough to force several flushes before forking,
/// which is what made the loss visible: without the fix it reported 200 leaks
/// instead of 20,100, and the ones it did report belonged to the child.
///
/// Expected: 20100 leaks, all blamed on this file, and the child's 500
/// allocations absent entirely.

#include <cstdio>
#include <cstdlib>

#include <sys/wait.h>
#include <unistd.h>

namespace {

/// Enough 16-byte blocks to push well past the agent's 1 MiB write buffer, so
/// a large part of the parent's trace is already on disk before the fork.
constexpr int kBeforeFork = 20000;
constexpr int kAfterFork = 100;

void leakBeforeForking() {
    for (int i = 0; i < kBeforeFork; ++i) {
        void* block = std::malloc(16);
        if (block == nullptr) {
            return;
        }
    }
}

void leakAfterReaping() {
    for (int i = 0; i < kAfterFork; ++i) {
        void* block = std::malloc(256);
        if (block == nullptr) {
            return;
        }
    }
}

/// The child role: allocates and leaks, and must not appear in the report.
int runAsChild() {
    for (int i = 0; i < 500; ++i) {
        void* block = std::malloc(64);
        if (block == nullptr) {
            return 1;
        }
    }
    std::printf("child leaked 500 blocks that must not be reported\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        return runAsChild();
    }

    leakBeforeForking();

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }
    if (pid == 0) {
        // Re-exec ourselves so the child is a genuinely new process image, which
        // is what re-runs the agent's library constructor.
        ::execl("/proc/self/exe", argv[0], "child", nullptr);
        std::perror("execl");
        ::_exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
    }

    leakAfterReaping();

    std::printf("parent leaked %d blocks\n", kBeforeFork + kAfterFork);
    return 0;
}
