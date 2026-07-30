/// @file RecordParser.cpp
/// @brief Record parsing -- and bug #1, an unfreed partial parse on the error path.

#include "poc/Record.hpp"

#include <cstdlib>
#include <cstring>

namespace poc {
namespace {

/// Both helpers are `static` (internal linkage), so they never reach the dynamic
/// symbol table. `dladdr` cannot see them; only the DWARF pass recovers their
/// names. That is deliberate -- it is what most application code looks like.

char* duplicateSpan(const char* begin, const char* end) {
    const std::size_t length = static_cast<std::size_t>(end - begin);
    char* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy == nullptr) {
        return nullptr;
    }
    std::memcpy(copy, begin, length);
    copy[length] = '\0';
    return copy;
}

/// Splits `key=value` into an owned Field.
[[nodiscard]] bool parseField(const char* begin, const char* end, Field& out) {
    const char* equals = static_cast<const char*>(std::memchr(begin, '=', end - begin));
    if (equals == nullptr || equals == begin) {
        return false;  // no key, or no '=' at all
    }

    out.name = duplicateSpan(begin, equals);
    out.value = duplicateSpan(equals + 1, end);
    return out.name != nullptr && out.value != nullptr;
}

}  // namespace

bool parseRecord(const char* text, Record& out) {
    if (text == nullptr) {
        return false;
    }

    const char* bar = std::strchr(text, '|');
    if (bar == nullptr) {
        return false;
    }

    out.id = std::strtol(text, nullptr, 10);
    out.fieldCount = 0;

    const char* cursor = bar + 1;
    while (*cursor != '\0' && out.fieldCount < Record::kMaxFields) {
        const char* semicolon = std::strchr(cursor, ';');
        const char* end = semicolon != nullptr ? semicolon : cursor + std::strlen(cursor);

        if (!parseField(cursor, end, out.fields[out.fieldCount])) {
            // ---------------------------------------------------------------
            // BUG #1: the early return.
            //
            // Every field parsed before this point is still owned by `out`, and
            // the caller -- seeing `false` -- throws the record away without
            // calling releaseRecord(). Whatever was parsed so far leaks.
            //
            // This is the single most common shape of leak in C-style code: the
            // happy path frees correctly, the error path forgets. It is also
            // invisible to review, because the leak is in the *caller's*
            // assumption, not in this function.
            // ---------------------------------------------------------------
            return false;
        }

        ++out.fieldCount;
        if (semicolon == nullptr) {
            break;
        }
        cursor = semicolon + 1;
    }

    return out.fieldCount > 0;
}

void releaseRecord(Record& record) {
    for (std::size_t i = 0; i < record.fieldCount; ++i) {
        std::free(record.fields[i].name);
        std::free(record.fields[i].value);
        record.fields[i] = Field{};
    }
    record.fieldCount = 0;
}

}  // namespace poc
