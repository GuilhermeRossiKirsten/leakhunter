/// A program that leaks and then calls _exit(), bypassing every destructor and
/// atexit handler.
///
/// Common in forking servers and in code that wants to skip cleanup on a fatal
/// error path. Like a crash, it never runs the agent's library destructor, so
/// the trace has to be flushed from the interposed `_exit` itself.
///
/// Expected: the 250 leaked blocks are reported, `run.exitCode` is 3, and the
/// data is marked partial.

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

namespace {

void leakThenBailOut() {
    for (int i = 0; i < 250; ++i) {
        void* block = std::malloc(128);
        if (block != nullptr) {
            static_cast<char*>(block)[0] = static_cast<char>(i);
        }
    }
}

}  // namespace

int main() {
    leakThenBailOut();

    std::printf("leaked 250 blocks, leaving via _exit()\n");
    std::fflush(stdout);

    _exit(3);
}
