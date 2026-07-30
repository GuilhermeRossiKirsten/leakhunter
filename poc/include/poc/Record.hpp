/// @file Record.hpp
/// @brief A parsed input record.

#pragma once

#include <cstddef>

namespace poc {

/// One field of a record. `name` and `value` are owned C strings, because that
/// is what the shape of a real C-interop parser tends to look like -- and it is
/// where ownership bugs live.
struct Field {
    char* name = nullptr;
    char* value = nullptr;
};

/// A record is a fixed-capacity array of owned fields.
struct Record {
    static constexpr std::size_t kMaxFields = 8;

    Field fields[kMaxFields]{};
    std::size_t fieldCount = 0;
    long id = 0;
};

/// Parses `id|key=value;key=value;...` into @p out.
/// @return true on success. On failure @p out is left partially built.
[[nodiscard]] bool parseRecord(const char* text, Record& out);

/// Releases every field @p record owns.
void releaseRecord(Record& record);

}  // namespace poc
