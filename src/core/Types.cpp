#include "leakhunter/core/Types.hpp"

#include <array>

#include <fmt/format.h>

namespace leakhunter {

std::string_view toString(AllocationKind kind) noexcept {
    switch (kind) {
        case AllocationKind::Malloc: return "malloc";
        case AllocationKind::Calloc: return "calloc";
        case AllocationKind::Realloc: return "realloc";
        case AllocationKind::AlignedAlloc: return "aligned_alloc";
        case AllocationKind::New: return "operator new";
        case AllocationKind::NewArray: return "operator new[]";
        case AllocationKind::Unknown: break;
    }
    return "unknown";
}

std::string_view toString(ReleaseKind kind) noexcept {
    switch (kind) {
        case ReleaseKind::Free: return "free";
        case ReleaseKind::Delete: return "operator delete";
        case ReleaseKind::DeleteArray: return "operator delete[]";
        case ReleaseKind::Realloc: return "realloc";
        case ReleaseKind::Unknown: break;
    }
    return "unknown";
}

std::string_view toSourceSpelling(AllocationKind kind) noexcept {
    switch (kind) {
        case AllocationKind::Malloc: return "malloc()";
        case AllocationKind::Calloc: return "calloc()";
        case AllocationKind::Realloc: return "realloc()";
        case AllocationKind::AlignedAlloc: return "aligned_alloc()";
        case AllocationKind::New: return "new";
        case AllocationKind::NewArray: return "new[]";
        case AllocationKind::Unknown: break;
    }
    return "an unknown allocator";
}

std::string_view toSourceSpelling(ReleaseKind kind) noexcept {
    switch (kind) {
        case ReleaseKind::Free: return "free()";
        case ReleaseKind::Delete: return "delete";
        case ReleaseKind::DeleteArray: return "delete[]";
        case ReleaseKind::Realloc: return "realloc()";
        case ReleaseKind::Unknown: break;
    }
    return "an unknown deallocator";
}

bool isCompatibleRelease(AllocationKind allocated, ReleaseKind released) noexcept {
    // Half the pair unobserved: never a finding. See the header for why.
    if (allocated == AllocationKind::Unknown || released == ReleaseKind::Unknown) {
        return true;
    }

    switch (allocated) {
        case AllocationKind::Malloc:
        case AllocationKind::Calloc:
        case AllocationKind::Realloc:
        case AllocationKind::AlignedAlloc:
            return released == ReleaseKind::Free || released == ReleaseKind::Realloc;

        case AllocationKind::New:
            return released == ReleaseKind::Delete;

        case AllocationKind::NewArray:
            return released == ReleaseKind::DeleteArray;

        case AllocationKind::Unknown:
            break;
    }
    return true;
}

std::string formatBytes(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 5> kUnits{"B", "KiB", "MiB", "GiB", "TiB"};

    if (bytes < 1024) {
        return fmt::format("{} B", bytes);
    }

    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    return fmt::format("{:.2f} {}", value, kUnits[unit]);
}

std::string StackFrame::describe() const {
    if (preciseName()) {
        return fmt::format("{} at {}:{}", function, file, line);
    }

    // No source location: the name came from dladdr and may be the nearest
    // exported symbol rather than the containing function. The offset says so.
    const std::string name = displayName();
    if (!module.empty()) {
        return fmt::format("{} ({}+0x{:x})", name, module, moduleOffset());
    }
    return fmt::format("{} (0x{:x})", name, address);
}

std::string StackFrame::displayName() const {
    if (function.empty()) {
        return fmt::format("<unknown>+0x{:x}", moduleOffset());
    }
    if (preciseName() || symbolOffset() == 0) {
        return function;
    }
    return fmt::format("{}+0x{:x}", function, symbolOffset());
}

}  // namespace leakhunter
