#include "leakhunter/core/ScopedTempFile.hpp"

#include <atomic>
#include <fstream>
#include <utility>

#include <fmt/format.h>

#if defined(_WIN32)
#include <process.h>
#define LH_GETPID _getpid
#else
#include <unistd.h>
#define LH_GETPID getpid
#endif

namespace leakhunter::core {
namespace fs = std::filesystem;
namespace {

std::atomic<unsigned> g_counter{0};

}  // namespace

ScopedTempFile::ScopedTempFile(std::string_view prefix) {
    std::error_code ec;
    const fs::path directory = fs::temp_directory_path(ec);

    const auto name = fmt::format("{}-{}-{}.lhtrace", prefix, static_cast<long>(LH_GETPID()),
                                  g_counter.fetch_add(1, std::memory_order_relaxed));

    path_ = (ec ? fs::path{"."} : directory) / name;
    owns_ = true;

    // Create it now so the child can open it for writing without racing.
    std::ofstream create(path_, std::ios::binary | std::ios::trunc);
}

ScopedTempFile::ScopedTempFile(fs::path path, bool ownsFile)
    : path_(std::move(path)), owns_(ownsFile) {}

ScopedTempFile::~ScopedTempFile() {
    if (owns_ && !path_.empty()) {
        std::error_code ec;
        fs::remove(path_, ec);  // best effort: a failure here must not throw
    }
}

ScopedTempFile::ScopedTempFile(ScopedTempFile&& other) noexcept
    : path_(std::move(other.path_)), owns_(std::exchange(other.owns_, false)) {}

ScopedTempFile& ScopedTempFile::operator=(ScopedTempFile&& other) noexcept {
    if (this != &other) {
        if (owns_ && !path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
        path_ = std::move(other.path_);
        owns_ = std::exchange(other.owns_, false);
    }
    return *this;
}

fs::path ScopedTempFile::release() noexcept {
    owns_ = false;
    return path_;
}

}  // namespace leakhunter::core
