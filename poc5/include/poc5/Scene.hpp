/// @file Scene.hpp
/// @brief A scene graph with owning parents and non-owning children.

#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace poc5 {

/// A C-allocated buffer owned by a unique_ptr with a custom deleter.
///
/// The deleter is the interesting part: the resource came from `malloc`, so it
/// must go back through `free`, and pairing them by hand is how `new[]`/`free`
/// mistakes happen. Stating the pairing once, in the type, makes it impossible
/// to get wrong at any of the call sites.
struct MallocDeleter {
    void operator()(void* pointer) const noexcept { std::free(pointer); }
};

using RawBuffer = std::unique_ptr<unsigned char, MallocDeleter>;

[[nodiscard]] RawBuffer makeRawBuffer(std::size_t bytes);

/// One node of a scene graph.
///
/// Parents own their children with `shared_ptr`; children point back with
/// `weak_ptr`. **That asymmetry is the whole design.** Two `shared_ptr`s
/// pointing at each other keep each other alive for ever -- a leak that smart
/// pointers do not prevent and that a leak detector reports exactly like any
/// other. `weak_ptr` on the way back is what breaks the cycle.
class Node : public std::enable_shared_from_this<Node> {
public:
    static std::shared_ptr<Node> create(std::string name, std::size_t payloadBytes);

    void addChild(const std::shared_ptr<Node>& child);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::size_t descendantCount() const;

    /// Empty when this is the root, or when the parent has already gone.
    [[nodiscard]] std::shared_ptr<Node> parent() const { return parent_.lock(); }

private:
    Node(std::string name, std::size_t payloadBytes);

    std::string name_;
    RawBuffer payload_;
    std::vector<std::shared_ptr<Node>> children_;
    std::weak_ptr<Node> parent_;  // NOT shared_ptr -- see the class comment
};

/// Builds a tree `breadth` wide and `depth` deep. Ownership lives in the
/// returned root; dropping it releases the whole graph.
[[nodiscard]] std::shared_ptr<Node> buildScene(std::size_t breadth, std::size_t depth,
                                               std::size_t payloadBytes);

}  // namespace poc5
