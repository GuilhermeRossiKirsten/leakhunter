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

        char* payload = cache.copyPayload(id);
        if (payload == nullptr) {
            continue;
        }

        // Count the non-padding bytes; a stand-in for whatever real work would
        // happen with the copy.
        totalBytes += std::strlen(payload) > 0 ? std::strlen(payload) : 0;
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
