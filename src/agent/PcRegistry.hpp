/// @file PcRegistry.hpp
/// @brief Lock-free set of distinct program counters seen during the run.
///
/// Symbolisation has to happen inside the target process -- only there can
/// dladdr() map an address to a module. But calling dladdr() from inside
/// `malloc` risks deadlocking against the dynamic loader's own lock.
///
/// The compromise: record which addresses were seen (cheap, lock-free, no
/// allocation) and resolve them all at shutdown, when the process is winding
/// down and the loader lock is free.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace leakhunter::agent {

class PcRegistry {
public:
    /// Power of two. 128 Ki slots covers ~64 Ki distinct call sites at a 50%
    /// load factor, comfortably more than real programs produce.
    static constexpr std::size_t kCapacity = 1U << 17;

    /// Inserts @p programCounter if absent. Safe from any thread, never blocks.
    void insert(std::uint64_t programCounter) noexcept;

    /// Invokes @p callback once per stored program counter. Single-threaded:
    /// only called during shutdown.
    template <typename Callback>
    void forEach(Callback&& callback) const {
        for (const auto& slot : slots_) {
            if (const std::uint64_t value = slot.load(std::memory_order_relaxed); value != 0) {
                callback(value);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

    /// Addresses dropped because the table filled up.
    [[nodiscard]] std::uint64_t overflowed() const noexcept {
        return overflowed_.load(std::memory_order_relaxed);
    }

private:
    /// Slots hold the program counter itself; 0 means empty. A program counter
    /// of 0 is never captured, so no sentinel collision is possible.
    std::atomic<std::uint64_t> slots_[kCapacity]{};
    std::atomic<std::size_t> count_{0};
    std::atomic<std::uint64_t> overflowed_{0};
};

}  // namespace leakhunter::agent
