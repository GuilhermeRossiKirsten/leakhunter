/// One allocation, never freed. The smallest interesting case.
///
/// Expected: 1 leak of 1024 bytes attributed to `allocateBuffer`.

#include <cstdio>
#include <cstdlib>

namespace {

char* allocateBuffer(std::size_t size) {
    auto* buffer = static_cast<char*>(std::malloc(size));
    if (buffer != nullptr) {
        buffer[0] = 'x';
    }
    return buffer;
}

}  // namespace

int main() {
    char* leaked = allocateBuffer(1024);
    std::printf("allocated 1024 bytes at %p and forgot about them\n",
                static_cast<void*>(leaked));

    // Deliberately no free(leaked).
    return 0;
}
