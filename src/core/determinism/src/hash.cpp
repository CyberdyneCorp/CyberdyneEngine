#include <cy/core/determinism/hash.h>

#include <cy/core/base/assert.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace cy::determinism {
namespace {

// Fixed, and not `cy::hash_bytes`: that function is seeded per process in development builds, which
// is right for a hash map and fatal for a state hash. See the header comment.
constexpr u64 kSecretA = 0xff51afd7ed558ccdULL;
constexpr u64 kSecretB = 0xc4ceb9fe1a85ec53ULL;
constexpr u64 kSecretC = 0x9e3779b97f4a7c15ULL;

[[nodiscard]] u64 fold_multiply(u64 a, u64 b) noexcept {
    const __uint128_t product = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    return static_cast<u64>(product) ^ static_cast<u64>(product >> 64U);
}

// Order matters: this is what makes a tree's children an ordered sequence rather than a set, and
// therefore what makes a walk in a different order a different hash.
[[nodiscard]] u64 combine(u64 accumulator, u64 value) noexcept {
    return fold_multiply(accumulator ^ kSecretA, value ^ kSecretB);
}

// A node's own identity is folded in before its contents, so that two nodes with identical contents
// at different positions do not hash alike.
[[nodiscard]] u64 seed_for(HashLevel level, u64 id) noexcept {
    return combine(fold_multiply(static_cast<u64>(level) ^ kSecretC, kStateHashVersion), id);
}

}  // namespace

const char* hash_level_name(HashLevel level) noexcept {
    switch (level) {
        case HashLevel::World:
            return "world";
        case HashLevel::Subsystem:
            return "subsystem";
        case HashLevel::Archetype:
            return "archetype";
        case HashLevel::Chunk:
            return "chunk";
        case HashLevel::Entity:
            return "entity";
        case HashLevel::Component:
            return "component";
        case HashLevel::Field:
            return "field";
    }
    return "unknown";
}

u64 fold_hash(u64 accumulator, u64 value) noexcept {
    return combine(accumulator, value);
}

Status StateHashTree::begin(HashLevel level, u64 id, const char* name) noexcept {
    if (stack_.size() >= kHashDepth) {
        return fail(ErrorCode::OutOfRange,
                    "state hash nesting is deeper than the seven levels the hierarchy defines");
    }

    HashNode node;
    node.level = level;
    node.id = id;
    node.name = name != nullptr ? name : "";
    node.hash = seed_for(level, id);
    node.parent = stack_.empty() ? HashNode::kNoNode : stack_.back();

    const auto index = static_cast<u32>(nodes_.size());
    if (Status pushed = nodes_.push_back(node); !pushed) {
        return pushed;
    }
    if (node.parent != HashNode::kNoNode) {
        HashNode& parent = nodes_[node.parent];
        if (parent.first_child == HashNode::kNoNode) {
            parent.first_child = index;
        } else {
            nodes_[parent.last_child].next_sibling = index;
        }
        parent.last_child = index;
        ++parent.child_count;
    }
    return stack_.push_back(index);
}

void StateHashTree::fold(u64 value) noexcept {
    if (stack_.empty()) {
        // A mix outside any node has nowhere to go. Silently dropping it would make a walk that
        // forgot its `begin()` produce a plausible hash of nothing; the assertion says so in the
        // two configurations that carry assertions, and the value is dropped in the other two
        // rather than corrupting an unrelated node.
        CY_ASSERT_MSG(false, "StateHashTree::mix_* called with no node open");
        return;
    }
    HashNode& node = nodes_[stack_.back()];
    node.hash = combine(node.hash, value);
}

void StateHashTree::mix_u64(u64 value) noexcept {
    fold(value);
}

void StateHashTree::mix_i64(i64 value) noexcept {
    fold(static_cast<u64>(value));
}

void StateHashTree::mix_f32(f32 value) noexcept {
    f32 normalised = value;
    if (normalised == 0.0F) {
        normalised = 0.0F;  // collapses -0.0, which compares equal and must therefore hash equal
    } else if (std::isnan(normalised)) {
        normalised = std::numeric_limits<f32>::quiet_NaN();
    }
    u32 bits = 0;
    std::memcpy(&bits, &normalised, sizeof(bits));
    fold(bits);
}

void StateHashTree::mix_f64(f64 value) noexcept {
    f64 normalised = value;
    if (normalised == 0.0) {
        normalised = 0.0;
    } else if (std::isnan(normalised)) {
        normalised = std::numeric_limits<f64>::quiet_NaN();
    }
    u64 bits = 0;
    std::memcpy(&bits, &normalised, sizeof(bits));
    fold(bits);
}

void StateHashTree::mix_text(const char* text) noexcept {
    if (text == nullptr) {
        fold(0);
        return;
    }
    u64 length = 0;
    u64 accumulator = kSecretC;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        accumulator = combine(accumulator, static_cast<u64>(static_cast<u8>(*cursor)));
        ++length;
    }
    // Length-prefixed in effect: the length is folded last, so "ab"+"c" and "a"+"bc" differ.
    fold(combine(accumulator, length));
}

Status StateHashTree::end() noexcept {
    if (stack_.empty()) {
        return fail(ErrorCode::InvalidArgument, "StateHashTree::end() with no node open");
    }
    const u32 index = stack_.back();
    stack_.pop_back();
    if (!stack_.empty()) {
        HashNode& parent = nodes_[stack_.back()];
        parent.hash = combine(parent.hash, nodes_[index].hash);
    }
    return ok();
}

u64 StateHashTree::root_hash() const noexcept {
    return nodes_.empty() ? 0 : nodes_[0].hash;
}

void StateHashTree::clear() noexcept {
    nodes_.clear();
    stack_.clear();
}

const HashNode* StateHashTree::find_child(const HashNode& parent, HashLevel level,
                                          u64 id) const noexcept {
    for (u32 index = parent.first_child; index != HashNode::kNoNode;
         index = nodes_[index].next_sibling) {
        const HashNode& child = nodes_[index];
        if (child.level == level && child.id == id) {
            return &child;
        }
    }
    return nullptr;
}

void StateHashTree::compare(const StateHashTree& left, const StateHashTree& right,
                            Divergence& out) noexcept {
    out = Divergence{};
    if (left.nodes_.empty() || right.nodes_.empty()) {
        out.diverged = left.nodes_.size() != right.nodes_.size();
        out.shape_mismatch = out.diverged;
        return;
    }

    const HashNode* a = left.nodes_.data();
    const HashNode* b = right.nodes_.data();
    if (a->hash == b->hash) {
        return;
    }

    out.diverged = true;
    // Descend while both sides have a child with the same (level, id) whose hashes differ. The
    // loop stops at the first node with no such child, which is either a leaf — the field that
    // differs — or a node whose children do not correspond, which is a shape mismatch.
    for (;;) {
        out.levels[out.depth] = a->level;
        out.ids[out.depth] = a->id;
        out.names[out.depth] = a->name;
        out.left = a->hash;
        out.right = b->hash;
        ++out.depth;

        if (a->child_count != b->child_count) {
            out.shape_mismatch = true;
            return;
        }
        if (a->child_count == 0 || out.depth >= kHashDepth) {
            return;
        }

        const HashNode* next_a = nullptr;
        const HashNode* next_b = nullptr;
        for (u32 index = a->first_child; index != HashNode::kNoNode;
             index = left.nodes_[index].next_sibling) {
            const HashNode& candidate = left.nodes_[index];
            const HashNode* peer = right.find_child(*b, candidate.level, candidate.id);
            if (peer == nullptr) {
                out.shape_mismatch = true;
                return;
            }
            if (peer->hash != candidate.hash) {
                next_a = &candidate;
                next_b = peer;
                break;
            }
        }
        if (next_a == nullptr) {
            // Every child agrees but the parents do not: the two walks folded the same children in
            // a different order. That is an ordering defect in the producer, not a state
            // divergence, and reporting it as a shape mismatch at this node is the honest answer.
            out.shape_mismatch = true;
            return;
        }
        a = next_a;
        b = next_b;
    }
}

}  // namespace cy::determinism
