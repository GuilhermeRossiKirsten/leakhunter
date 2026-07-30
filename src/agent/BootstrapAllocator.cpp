#include "BootstrapAllocator.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

namespace leakhunter::agent::bootstrap {
namespace {

constexpr std::size_t kAlignment = alignof(std::max_align_t);

/// Stored immediately before every block so sizeOf() can answer without a map.
/// Padded to kAlignment so the payload keeps maximal alignment.
struct alignas(kAlignment) BlockHeader {
    std::size_t size;
    std::size_t magic;
};

constexpr std::size_t kHeaderMagic = 0x4C48'424F'4F54'0001ULL;  // "LHBOOT"

// Zero-initialised, so it lives in BSS and costs nothing until touched.
alignas(kAlignment) unsigned char g_arena[kCapacity];
std::atomic<std::size_t> g_offset{0};

[[nodiscard]] constexpr std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

void* allocate(std::size_t size) noexcept {
    const std::size_t total = alignUp(sizeof(BlockHeader) + size, kAlignment);

    // Lock-free bump: several threads may be in the loader concurrently.
    std::size_t current = g_offset.load(std::memory_order_relaxed);
    std::size_t next = 0;
    do {
        if (total > kCapacity || current > kCapacity - total) {
            return nullptr;  // exhausted; the caller reports a hard failure
        }
        next = current + total;
    } while (!g_offset.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                             std::memory_order_relaxed));

    auto* header = reinterpret_cast<BlockHeader*>(g_arena + current);
    header->size = size;
    header->magic = kHeaderMagic;

    return g_arena + current + sizeof(BlockHeader);
}

void* allocateZeroed(std::size_t count, std::size_t size) noexcept {
    // Overflow check: callers reaching here are the dynamic loader, but a
    // wrapped allocator must never be the weak link.
    if (size != 0 && count > static_cast<std::size_t>(-1) / size) {
        return nullptr;
    }

    const std::size_t total = count * size;
    void* block = allocate(total);
    if (block != nullptr) {
        std::memset(block, 0, total);
    }
    return block;
}

bool owns(const void* pointer) noexcept {
    const auto* address = static_cast<const unsigned char*>(pointer);
    return address >= g_arena && address < g_arena + kCapacity;
}

std::size_t sizeOf(const void* pointer) noexcept {
    if (!owns(pointer) ||
        static_cast<const unsigned char*>(pointer) < g_arena + sizeof(BlockHeader)) {
        return 0;
    }

    const auto* header = reinterpret_cast<const BlockHeader*>(
        static_cast<const unsigned char*>(pointer) - sizeof(BlockHeader));
    return header->magic == kHeaderMagic ? header->size : 0;
}

std::size_t used() noexcept {
    return g_offset.load(std::memory_order_relaxed);
}

}  // namespace leakhunter::agent::bootstrap
