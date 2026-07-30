#include "poc5/Scene.hpp"

#include <new>
#include <utility>

namespace poc5 {

RawBuffer makeRawBuffer(std::size_t bytes) {
    // The one place malloc appears. Everything downstream holds a RawBuffer and
    // cannot forget how it has to be released.
    auto* raw = static_cast<unsigned char*>(std::malloc(bytes));
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    for (std::size_t i = 0; i < bytes; ++i) {
        raw[i] = static_cast<unsigned char>(i & 0xFF);
    }
    return RawBuffer{raw};
}

Node::Node(std::string name, std::size_t payloadBytes)
    : name_(std::move(name)), payload_(makeRawBuffer(payloadBytes)) {}

std::shared_ptr<Node> Node::create(std::string name, std::size_t payloadBytes) {
    // The constructor is private, so make_shared cannot reach it; new + a
    // shared_ptr is the standard way round that. The control block is a second
    // allocation here rather than make_shared's single one -- a real cost,
    // accepted for a private constructor.
    return std::shared_ptr<Node>(new Node(std::move(name), payloadBytes));
}

void Node::addChild(const std::shared_ptr<Node>& child) {
    if (!child) {
        return;
    }
    child->parent_ = weak_from_this();  // weak: the child must not own us back
    children_.push_back(child);
}

std::size_t Node::descendantCount() const {
    std::size_t count = children_.size();
    for (const std::shared_ptr<Node>& child : children_) {
        count += child->descendantCount();
    }
    return count;
}

std::shared_ptr<Node> buildScene(std::size_t breadth, std::size_t depth,
                                 std::size_t payloadBytes) {
    auto root = Node::create("root", payloadBytes);

    std::vector<std::shared_ptr<Node>> frontier{root};
    for (std::size_t level = 0; level < depth; ++level) {
        std::vector<std::shared_ptr<Node>> next;
        for (const std::shared_ptr<Node>& parent : frontier) {
            for (std::size_t i = 0; i < breadth; ++i) {
                auto child = Node::create(parent->name() + "." + std::to_string(i), payloadBytes);
                parent->addChild(child);
                next.push_back(std::move(child));
            }
        }
        frontier = std::move(next);
    }

    return root;
}

}  // namespace poc5
