#include "PcRegistry.hpp"

namespace leakhunter::agent {
namespace {

/// Splitmix64 finaliser: cheap, no multiplication chains long enough to matter
/// on the allocation fast path, and good enough spread for code addresses
/// (which cluster heavily in the low bits).
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xFF51'AFD7'ED55'8CCDULL;
    value ^= value >> 33;
    value *= 0xC4CE'B9FE'1A85'EC53ULL;
    value ^= value >> 33;
    return value;
}

/// Probe budget before declaring the table full. Bounded so the fast path has a
/// hard upper bound on cost regardless of load factor.
constexpr std::size_t kMaxProbes = 64;

}  // namespace

void PcRegistry::insert(std::uint64_t programCounter) noexcept {
    if (programCounter == 0) {
        return;
    }

    std::size_t index = static_cast<std::size_t>(mix(programCounter)) & (kCapacity - 1);

    for (std::size_t probe = 0; probe < kMaxProbes; ++probe) {
        std::uint64_t existing = slots_[index].load(std::memory_order_relaxed);

        if (existing == programCounter) {
            return;  // already recorded, the common case after warm-up
        }

        if (existing == 0) {
            // Claim the slot. On a lost race, re-read and continue: another
            // thread either stored our value (done) or a different one (probe on).
            if (slots_[index].compare_exchange_strong(existing, programCounter,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_relaxed)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (existing == programCounter) {
                return;
            }
        }

        index = (index + 1) & (kCapacity - 1);
    }

    overflowed_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace leakhunter::agent
