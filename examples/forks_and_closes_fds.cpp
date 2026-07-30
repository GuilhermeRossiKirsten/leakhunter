/// Two things that used to break tracing, in one target.
///
/// **1. `fork()` without `exec()`.** The atfork handler stops the child from
/// *recording*, which is not enough on its own. The child also inherits a copy
/// of the parent's unflushed buffer -- file header included -- and the same open
/// file description, hence the same file *offset*. So anything that flushed in
/// the child wrote a second copy of the parent's records and pushed the parent's
/// next write further into the file. The host read the first copy, hit the
/// second header, and stopped: 50 leaks reported out of 100, no end marker.
///
/// **2. The daemonisation idiom.** `for (fd = 3; fd < 256; ++fd) close(fd)` is
/// how a daemon sheds inherited descriptors, and it closed the agent's trace
/// descriptor too. Writes then failed with `EBADF` and the run produced an empty
/// trace, which the host blamed on static linking. The descriptor now sits above
/// that loop's range, and a failure that happens anyway is named rather than
/// guessed at.
///
/// Expected: exactly 400 leaks of 128 bytes, all blamed on this file, with the
/// child's 300 absent and no dropped records.

#include <cstdio>
#include <cstdlib>

#include <sys/wait.h>
#include <unistd.h>

namespace {

/// Storing through a volatile pointer stops the compiler eliding the calls.
/// C++ permits eliding allocations and GCC does it at -O1, which is enough to
/// make a test like this quietly measure nothing.
void* volatile g_sink = nullptr;

void leakBlocks(int count, std::size_t size) {
    for (int i = 0; i < count; ++i) {
        g_sink = std::malloc(size);
    }
}

/// Sheds every descriptor a daemon might have inherited.
void closeInheritedDescriptors() {
    for (int fd = 3; fd < 256; ++fd) {
        ::close(fd);
    }
}

}  // namespace

int main() {
    leakBlocks(100, 128);

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }
    if (pid == 0) {
        // Same image, so the atfork handler applies. These must not be reported,
        // and must not corrupt the parent's trace on the way out.
        leakBlocks(300, 4096);
        std::printf("child leaked 300 blocks that must not be reported\n");
        ::_exit(0);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
    }

    leakBlocks(100, 128);
    closeInheritedDescriptors();
    leakBlocks(200, 128);

    std::printf("parent leaked 400 blocks of 128 across a fork and an fd purge\n");
    return 0;
}
