/// A program that leaks and then dies before it can shut down cleanly.
///
/// This is the case a leak hunter is most often reached for, and the one most
/// easily lost: the agent's library destructor never runs, so without a
/// fatal-signal handler every buffered record would be discarded.
///
/// Expected: the 400 leaked blocks are still reported, `run.terminatingSignal`
/// is 11 (SIGSEGV), and `summary.droppedRecords` is non-zero to mark the data
/// as partial (there is no end marker and no symbol table).

#include <cstdio>
#include <cstdlib>

namespace {

void leakBeforeDying() {
    for (int i = 0; i < 400; ++i) {
        void* block = std::malloc(256);
        // Keep the compiler from eliding the allocation.
        if (block != nullptr) {
            static_cast<char*>(block)[0] = static_cast<char>(i);
        }
    }
}

}  // namespace

int main() {
    leakBeforeDying();

    std::printf("leaked 400 blocks, now crashing on purpose\n");
    std::fflush(stdout);

    // Deliberate null dereference. volatile stops the compiler from turning
    // this into a trap instruction with no signal.
    int* volatile nowhere = nullptr;
    *nowhere = 42;

    return 0;
}
