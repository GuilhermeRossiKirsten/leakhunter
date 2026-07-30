/// @file main.cpp
/// @brief docindex -- a small document indexer with four planted defects.
///
/// The point of this program is that it *works*. It processes its input, prints
/// a plausible summary and exits 0. Nothing crashes, nothing misbehaves, and
/// `valgrind`-free CI would pass it. The four defects are the kind that ship:
///
///   1. RecordParser.cpp   -- partial parse leaked on the error path
///   2. DocumentCache.cpp  -- evictAll() clears the vector, not the objects
///   3. ReportBuilder.cpp  -- `new char[]` released with free() (undefined behaviour)
///   4. IndexWorker.cpp    -- per-batch scratch buffer leaked, on worker threads
///
/// Run it under LeakHunter and each one is named, with its file and line.

#include <cstdio>
#include <string>
#include <vector>

#include "poc/DocumentCache.hpp"
#include "poc/IndexWorker.hpp"
#include "poc/Record.hpp"
#include "poc/ReportBuilder.hpp"

namespace {

constexpr int kTotalRecords = 500;
constexpr int kMalformedEvery = 10;   ///< every 10th record is rejected
constexpr long kDocumentCount = 200;
constexpr std::size_t kPayloadBytes = 512;
constexpr std::size_t kWorkerThreads = 4;
constexpr std::size_t kTasksPerThread = 25;

/// Builds an input line. Every kMalformedEvery-th one has a field with no '=',
/// which is what drives the parser down its error path.
std::string makeInputLine(int index) {
    std::string line = std::to_string(index) + "|kind=article;lang=en";
    if (index % kMalformedEvery == 0) {
        line += ";this-field-has-no-equals-sign";
    } else {
        line += ";status=ok";
    }
    return line;
}

/// Stage 1: parse the input. Rejected records are dropped, which is where the
/// partially-built fields go missing.
int ingest() {
    int accepted = 0;
    for (int i = 1; i <= kTotalRecords; ++i) {
        const std::string line = makeInputLine(i);

        poc::Record record;
        if (!poc::parseRecord(line.c_str(), record)) {
            // The record is malformed, so it is discarded. releaseRecord() is
            // not called -- the caller has no idea anything was allocated.
            continue;
        }

        ++accepted;
        poc::releaseRecord(record);  // the happy path is correct
    }
    return accepted;
}

}  // namespace

int main() {
    std::printf("docindex starting\n");

    const int accepted = ingest();
    std::printf("  ingested %d of %d records (%d rejected)\n", accepted, kTotalRecords,
                kTotalRecords - accepted);

    poc::DocumentCache cache;
    for (long id = 1; id <= kDocumentCount; ++id) {
        cache.insert(poc::buildDocument(id, kPayloadBytes));
    }
    std::printf("  cached %zu documents\n", cache.size());

    const std::string summary = poc::summarise(cache, 1, 8);
    std::printf("  %s\n", summary.c_str());

    const std::size_t batches = poc::runIndexer(kWorkerThreads, kTasksPerThread);
    std::printf("  indexed %zu batches on %zu threads\n", batches, kWorkerThreads);

    // "Freeing" the cache. This is the line everyone reads as cleanup.
    cache.evictAll();
    std::printf("  cache evicted\n");

    std::printf("docindex finished cleanly\n");
    return 0;
}
