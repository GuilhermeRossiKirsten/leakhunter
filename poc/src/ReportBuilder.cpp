/// @file ReportBuilder.cpp
/// @brief Summaries -- and bug #3, a `new[]` buffer released with free().

#include "poc/ReportBuilder.hpp"

#include <cstdlib>
#include <cstring>

namespace poc {

std::string summarise(const DocumentCache& cache, long firstId, std::size_t count) {
    std::size_t totalBytes = 0;
    std::size_t seen = 0;

    for (std::size_t offset = 0; offset < count; ++offset) {
        const long id = firstId + static_cast<long>(offset);

        std::size_t payloadBytes = 0;
        char* payload = cache.copyPayload(id, payloadBytes);
        if (payload == nullptr) {
            continue;
        }

        // Use the length the cache reported. This used to be `strlen(payload)`,
        // which read 513 bytes out of a 512-byte block -- the payload is bytes,
        // not a string, and nothing NUL-terminates it. LeakHunter cannot see a
        // read past the end of a live block; AddressSanitizer found it in 0.07s.
        // Written up in docs/DETECTION.md, because a blind spot is worth more
        // as a documented example than as a quietly fixed commit.
        totalBytes += payloadBytes;
        ++seen;

        // -------------------------------------------------------------------
        // BUG #3: copyPayload() returns memory from `new char[]`.
        //
        // free() does not run operator delete[], and on a type with a
        // destructor it would skip that too. Here it happens to work, because
        // char has no destructor and glibc's free() accepts what operator new[]
        // handed out. It will keep working right up until the allocator changes,
        // or someone turns the payload into a type with a destructor.
        //
        // Note that no compiler warns: the allocation is in DocumentCache.cpp
        // and the release is here. -Wmismatched-new-delete only sees one TU at a
        // time. This cross-translation-unit blindness is the whole reason a
        // run-time tool earns its place.
        //
        // The memory IS returned -- this is not a leak. It is undefined
        // behaviour, which is why it shows up in a section of its own.
        // -------------------------------------------------------------------
        std::free(payload);
    }

    return "summarised " + std::to_string(seen) + " document(s), " + std::to_string(totalBytes) +
           " payload bytes";
}

}  // namespace poc
