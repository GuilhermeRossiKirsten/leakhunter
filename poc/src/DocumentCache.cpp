/// @file DocumentCache.cpp
/// @brief The cache -- and bug #2, an evictAll() that forgets its pointers.

#include "poc/DocumentCache.hpp"

#include <algorithm>
#include <cstring>

namespace poc {

Document* buildDocument(long id, std::size_t payloadBytes) {
    auto* document = new Document();
    document->id = id;
    document->title = "document-" + std::to_string(id);
    document->payloadBytes = payloadBytes;

    // new char[] on purpose: it pairs with delete[], and only with delete[].
    document->payload = new char[payloadBytes];
    std::memset(document->payload, 'x', payloadBytes);
    return document;
}

DocumentCache::~DocumentCache() {
    // The destructor is correct. It is never reached in the demo, because the
    // cache is evicted first -- which is exactly why the bug below survives:
    // "the destructor cleans up" is true and irrelevant.
    for (Document* document : documents_) {
        delete[] document->payload;
        delete document;
    }
}

void DocumentCache::insert(Document* document) {
    if (document != nullptr) {
        documents_.push_back(document);
    }
}

const Document* DocumentCache::find(long id) const {
    const auto match = std::find_if(documents_.begin(), documents_.end(),
                                    [id](const Document* d) { return d->id == id; });
    return match != documents_.end() ? *match : nullptr;
}

void DocumentCache::evictAll() {
    // -----------------------------------------------------------------------
    // BUG #2: clear() releases the vector's storage, not the objects it points
    // at. Every Document and every payload buffer is now unreachable and
    // unfreed.
    //
    // The line reads as if it does the right thing, which is what makes it last.
    // The fix is the loop in the destructor above -- or, better, holding
    // std::unique_ptr<Document> so the question cannot come up.
    // -----------------------------------------------------------------------
    documents_.clear();
}

char* DocumentCache::copyPayload(long id, std::size_t& bytes) const {
    bytes = 0;
    const Document* document = find(id);
    if (document == nullptr) {
        return nullptr;
    }
    bytes = document->payloadBytes;

    // Allocated with new[], so the caller must use delete[]. See bug #3 in
    // ReportBuilder.cpp, in a different translation unit -- which is precisely
    // why no compiler warns about it.
    char* copy = new char[document->payloadBytes];
    std::memcpy(copy, document->payload, document->payloadBytes);
    return copy;
}

}  // namespace poc
