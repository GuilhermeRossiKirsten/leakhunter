/// @file DocumentCache.hpp
/// @brief An in-memory document cache holding raw pointers.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace poc {

struct Document {
    long id = 0;
    std::string title;
    std::size_t payloadBytes = 0;
    char* payload = nullptr;  ///< owned
};

/// Caches documents by id.
///
/// Holds `Document*` rather than `std::unique_ptr<Document>` on purpose: this is
/// the interface a decade-old codebase actually has, and the ownership is
/// therefore a convention rather than something the type system enforces.
class DocumentCache {
public:
    ~DocumentCache();

    /// Takes ownership of a newly built document.
    void insert(Document* document);

    [[nodiscard]] const Document* find(long id) const;
    [[nodiscard]] std::size_t size() const noexcept { return documents_.size(); }

    /// Empties the cache.
    void evictAll();

    /// Hands out a raw copy of a document's payload for the caller to release.
    /// The contract is documented; whether the caller honours it is bug #3.
    ///
    /// @param bytes receives the size of the copy. The payload is **not**
    ///        NUL-terminated -- it is bytes, not a string -- so the caller has
    ///        no way to discover the length on its own. Returning it is not a
    ///        convenience: without it the only thing a caller can reach for is
    ///        strlen, which reads past the end. AddressSanitizer caught exactly
    ///        that here; see docs/DETECTION.md.
    [[nodiscard]] char* copyPayload(long id, std::size_t& bytes) const;

private:
    std::vector<Document*> documents_;
};

/// Builds a document with an owned payload of @p payloadBytes.
[[nodiscard]] Document* buildDocument(long id, std::size_t payloadBytes);

}  // namespace poc
