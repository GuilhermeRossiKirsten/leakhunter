/// A program that leaks nothing. The negative control: if LeakHunter reports a
/// leak here, it is reporting noise.
///
/// Expected: exit code 0 and an empty leak list.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

int main() {
    for (int round = 0; round < 100; ++round) {
        void* raw = std::malloc(1024);
        std::free(raw);

        auto owned = std::make_unique<int[]>(256);
        owned[0] = round;

        std::vector<std::string> strings;
        for (int i = 0; i < 20; ++i) {
            strings.emplace_back("a reasonably long string that will not fit in the SSO buffer");
        }
    }

    std::printf("allocated and released everything\n");
    return 0;
}
