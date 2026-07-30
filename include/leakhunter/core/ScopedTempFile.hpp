/// @file ScopedTempFile.hpp
/// @brief RAII wrapper around the intermediate trace file.

#pragma once

#include <filesystem>
#include <string_view>

namespace leakhunter::core {

/// Owns a path in the temp directory and removes it on destruction, including
/// on the error paths -- the trace of a long run can be hundreds of megabytes,
/// so leaving one behind after a failure is not acceptable.
///
/// Move-only; `release()` opts out of deletion (used by --keep-trace).
class ScopedTempFile {
public:
    /// Creates an empty file named "<prefix>-<pid>-<counter>.lhtrace".
    explicit ScopedTempFile(std::string_view prefix = "leakhunter");

    /// Adopts an existing path without creating it.
    explicit ScopedTempFile(std::filesystem::path path, bool ownsFile);

    ~ScopedTempFile();

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ScopedTempFile(ScopedTempFile&& other) noexcept;
    ScopedTempFile& operator=(ScopedTempFile&& other) noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Stops the destructor from deleting the file and returns its path.
    std::filesystem::path release() noexcept;

private:
    std::filesystem::path path_;
    bool owns_ = false;
};

}  // namespace leakhunter::core
