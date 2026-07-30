/// @file main.cpp
/// @brief The negative control: a program that allocates hard and leaks nothing.
///
/// Every other demonstration here is planted with a defect. This one is not,
/// and that makes it the most important of them: a leak detector that reports
/// something on clean code is worse than useless, because people switch it off.
///
/// It is not clean by being simple. It allocates tens of thousands of times
/// through the paths that most often go wrong:
///
///   * a raw `malloc` buffer, released by a `unique_ptr` with a custom deleter;
///   * a `shared_ptr` scene graph whose back-references are `weak_ptr`, because
///     a `shared_ptr` cycle leaks exactly like a forgotten `free`;
///   * an exception thrown mid-operation, with the stack unwound by RAII;
///   * `std::pmr` with a monotonic buffer resource, which allocates upfront and
///     releases in one go;
///   * ownership moved through factories and containers.
///
/// Expected: 0 leaks, 0 mismatched frees, exit code 0.

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "poc5/Scene.hpp"

namespace {

/// Throws part-way through, with two live owning handles on the stack.
///
/// The pre-RAII version of this is the classic leak: allocate, allocate, throw,
/// and the first buffer is gone for ever. Here both destructors run during
/// unwinding and nothing survives.
void operationThatThrows(std::size_t bytes) {
    poc5::RawBuffer scratch = poc5::makeRawBuffer(bytes);
    auto node = poc5::Node::create("transient", bytes);

    if (scratch != nullptr) {
        throw std::runtime_error("simulated failure half way through");
    }
}

/// A pmr region. The resource owns one upstream block and hands slices of it
/// out; everything is released when it goes out of scope, in a single free.
[[nodiscard]] std::size_t summariseWithArena(std::size_t records) {
    std::array<std::byte, 64 * 1024> stackBuffer{};
    std::pmr::monotonic_buffer_resource arena(stackBuffer.data(), stackBuffer.size());

    std::pmr::vector<std::pmr::string> labels(&arena);
    labels.reserve(records);
    for (std::size_t i = 0; i < records; ++i) {
        // Long enough to defeat the small-string optimisation, so this really
        // does go through the allocator.
        //
        // No allocator argument here: a pmr container propagates its own to the
        // elements it constructs. Passing one as well is what
        // uses-allocator construction then cannot make sense of.
        const std::string text =
            "telemetry-record-with-a-deliberately-long-name-" + std::to_string(i);
        labels.emplace_back(text.c_str());
    }

    std::size_t total = 0;
    for (const std::pmr::string& label : labels) {
        total += label.size();
    }
    return total;
}

/// Ownership moved out of a factory, through a container, and back.
[[nodiscard]] std::vector<std::unique_ptr<std::string>> makeOwnedStrings(std::size_t count) {
    std::vector<std::unique_ptr<std::string>> owned;
    owned.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        owned.push_back(std::make_unique<std::string>(
            "owned-string-long-enough-to-allocate-" + std::to_string(i)));
    }
    return owned;  // moved, not copied
}

/// Reads through a span, which owns nothing and must allocate nothing.
[[nodiscard]] std::size_t checksum(std::span<const std::unique_ptr<std::string>> values) {
    std::size_t total = 0;
    for (const auto& value : values) {
        total += value->size();
    }
    return total;
}

}  // namespace

int main() {
    std::printf("clean-app starting\n");

    // 1. A scene graph: 1 + 6 + 36 + 216 = 259 nodes, each with a raw buffer.
    //    Dropping the root releases every one of them.
    std::size_t nodes = 0;
    {
        const auto root = poc5::buildScene(/*breadth=*/6, /*depth=*/3, /*payloadBytes=*/512);
        nodes = root->descendantCount() + 1;

        // The back-reference resolves while the parent is alive, and is empty
        // once it is not -- which is what stops the cycle.
        std::printf("  scene: %zu nodes, root parent is %s\n", nodes,
                    root->parent() ? "set" : "empty (as it should be)");
    }

    // 2. An exception through two owning handles.
    std::size_t caught = 0;
    for (int attempt = 0; attempt < 500; ++attempt) {
        try {
            operationThatThrows(1024);
        } catch (const std::runtime_error&) {
            ++caught;
        }
    }
    std::printf("  unwound %zu exceptions with live owners on the stack\n", caught);

    // 3. A pmr arena over a stack buffer.
    const std::size_t labelBytes = summariseWithArena(2000);
    std::printf("  arena summarised %zu label bytes\n", labelBytes);

    // 4. Ownership moved out of a factory and read through a non-owning view.
    const auto owned = makeOwnedStrings(3000);
    std::printf("  %zu owned strings, %zu bytes total\n", owned.size(), checksum(owned));

    std::printf("clean-app finished; every allocation was released\n");
    return 0;
}
