#pragma once
// Hierarchical state hashing, and the narrowing that makes a divergence a named field. Task 4.2.6.
//
// `simulation-and-determinism` — "Hierarchical state hashing": the hash is hierarchical — world,
// subsystem, archetype, chunk, entity, component, field — a divergence is narrowable by descending
// the hierarchy "so that the result of a mismatch is a named field on a named entity rather than a
// statement that two numbers differ", only declared authoritative fields are covered, and raw
// memory, padding and derived data are not hashed.
//
// --- WHAT THE TREE IS FOR ------------------------------------------------------------------------
//
// A flat hash answers "did we diverge". A tree answers "where", and that is the entire reason to
// pay for one: two runs produce two trees with the same *shape* (the walk is deterministic, so the
// shape is a function of the world's contents, not of its history), and comparing them top-down
// stops at the first node whose hash differs and descends only into that. Narrowing is therefore
// O(depth), not O(state).
//
// --- WHAT IS DELIBERATELY NOT IN THE M2 WALK -----------------------------------------------------
//
// **The chunk level is in the enum and not in the walk**, and that is a decision rather than an
// omission. Chunk assignment is allocator and insertion history; `simulation-and-determinism`
// requires that "determinism SHALL NOT depend on allocator history or archetype creation order by
// accident" and has its validator *deliberately perturb* chunk assignment between runs. A hash with
// a chunk level in it would therefore differ between two runs that agree about every value, which
// is the opposite of what a hash is for. The level stays in the enum because an incremental scheme
// — maintaining a per-chunk subtree hash and re-folding them in a stable order — is the shape M9
// will want, and it can use the level without changing every consumer of this file.
//
// **Nothing here is incremental.** The requirement says subtree hashes SHOULD be maintained
// incrementally "where practical"; at M2 a hash is a full walk. The cost is measured by the runtime
// and reported, so the decision to make it incremental will be taken against a number.
//
// --- THE MIXING ----------------------------------------------------------------------------------
//
// Fixed constants, and deliberately NOT `cy::hash_bytes`, which is seeded per process in
// development builds. A state hash keyed by that would differ between two runs of the same binary,
// which is precisely the failure it exists to detect. Scalars are mixed by *value*, one width at a
// time, so padding is never read and -0.0 hashes as 0.0 — the two ways raw memory quietly makes a
// hash wrong.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::determinism {

/// Bumped when the mixing below changes. Two captures with different versions are incomparable, and
/// saying so is better than reporting a divergence at the root.
inline constexpr u32 kStateHashVersion = 1;

/// The levels `simulation-and-determinism` names, outermost first. `Chunk` is present and unused;
/// see the header comment.
enum class HashLevel : u8 {
    World = 0,
    Subsystem,
    Archetype,
    Chunk,
    Entity,
    Component,
    Field,
};

const char* hash_level_name(HashLevel level) noexcept;

/// Fold one value into an accumulator, with the same mixing the tree uses.
///
/// Exposed because a producer sometimes has to build a stable *key* out of several numbers before
/// it can order its walk — the runtime's archetype key is one — and a second mixing function
/// written beside this one would be a second thing to keep in step. Order matters.
[[nodiscard]] u64 fold_hash(u64 accumulator, u64 value) noexcept;

inline constexpr u32 kHashDepth = 7;

/// One node of the tree. `name` is a literal or storage outliving the tree — a component's name, a
/// subsystem's — and is metadata: `id` is what the comparison reports, because a name can change
/// without the state changing.
struct HashNode {
    HashLevel level = HashLevel::World;
    u64 id = 0;
    const char* name = "";
    u64 hash = 0;
    u32 parent = kNoNode;
    /// Children as a singly linked list, NOT as a contiguous range. The tree is built by a
    /// depth-first walk, so a node's children are separated in `nodes_` by whole subtrees — a
    /// first-child-plus-count encoding reads the wrong nodes the moment any child has children of
    /// its own, which is every level of a real state hash. `last_child` is kept so that appending
    /// is O(1) rather than a walk of the chain.
    u32 first_child = kNoNode;
    u32 last_child = kNoNode;
    u32 next_sibling = kNoNode;
    u32 child_count = 0;

    static constexpr u32 kNoNode = 0xFFFFFFFFU;
};

/// Where two trees first disagree, as a path from the root.
struct Divergence {
    /// The path, root first. `depth` entries are valid.
    HashLevel levels[kHashDepth] = {};
    u64 ids[kHashDepth] = {};
    const char* names[kHashDepth] = {};
    u32 depth = 0;

    /// The two hashes at the deepest node that could be compared.
    u64 left = 0;
    u64 right = 0;

    /// False when the trees agreed. Every other field is then meaningless.
    bool diverged = false;

    /// True when the descent stopped because the two trees have different *shapes* at this node —
    /// an entity present in one run and absent in the other, say. Distinguished from a value
    /// mismatch because the two have completely different causes.
    bool shape_mismatch = false;
};

/// A hierarchical hash, built by a walk and compared against another.
///
/// Built with `begin`/`mix_*`/`end` in the order the walk visits, which is why the walk's order has
/// to be stable — a tree whose children are in a different order is a different tree even when
/// every value agrees. Every producer of one is responsible for that ordering, and
/// `simulation-and-determinism`'s "Stable iteration and tie-breaking" is the rule they follow
/// (ordering.h).
class StateHashTree {
public:
    explicit StateHashTree(Allocator& allocator) noexcept : nodes_(allocator), stack_(allocator) {}

    StateHashTree(const StateHashTree&) = delete;
    StateHashTree& operator=(const StateHashTree&) = delete;

    /// Open a node. Nodes nest; the level is recorded rather than derived from the depth, because
    /// the walk legitimately skips levels (there is no `Subsystem` between a world and its
    /// archetypes).
    [[nodiscard]] Status begin(HashLevel level, u64 id, const char* name) noexcept;

    /// Fold a leaf value into the open node. One overload per width, and no `const void*` overload
    /// taking a size: an interface that accepts a struct's address and its `sizeof` is an interface
    /// that hashes padding, and `simulation-and-determinism` lists that among the forbidden
    /// patterns.
    void mix_u64(u64 value) noexcept;
    void mix_i64(i64 value) noexcept;
    /// Zero is normalised so that -0.0 and 0.0, which compare equal, hash equal. A signalling NaN
    /// is normalised to a quiet one for the same reason: two runs that both produced "not a number"
    /// have not diverged, and the payload bits are not simulation state. A non-finite value in an
    /// authoritative field is still a defect; detecting it is the M9 profile's, and this only
    /// stops it from producing a divergence report that names the wrong thing.
    void mix_f32(f32 value) noexcept;
    void mix_f64(f64 value) noexcept;
    /// Text: a name, an identifier. Length-prefixed, so "ab" + "c" and "a" + "bc" differ.
    void mix_text(const char* text) noexcept;

    /// Close the open node, folding its hash into its parent's.
    [[nodiscard]] Status end() noexcept;

    /// The root's hash. Zero for an empty tree, and `open()` is how a caller tells the difference
    /// between "empty" and "still being built".
    [[nodiscard]] u64 root_hash() const noexcept;
    [[nodiscard]] bool open() const noexcept { return !stack_.empty(); }
    [[nodiscard]] u32 node_count() const noexcept { return static_cast<u32>(nodes_.size()); }
    [[nodiscard]] const HashNode& node(u32 index) const noexcept { return nodes_[index]; }
    [[nodiscard]] Span<const HashNode> nodes() const noexcept { return nodes_.span(); }

    void clear() noexcept;

    /// Descend both trees together and report where they first disagree.
    ///
    /// The comparison matches children by `(level, id)` rather than by position, so a run that
    /// created one extra entity reports *that entity* as a shape mismatch rather than reporting
    /// every entity after it as a value mismatch.
    static void compare(const StateHashTree& left, const StateHashTree& right,
                        Divergence& out) noexcept;

private:
    void fold(u64 value) noexcept;
    [[nodiscard]] const HashNode* find_child(const HashNode& parent, HashLevel level,
                                             u64 id) const noexcept;

    Array<HashNode> nodes_;
    /// Indices into `nodes_`, innermost last. A separate stack rather than walking parents, so that
    /// `mix_*` is a single indexed store.
    Array<u32> stack_;
};

/// How often the hash is taken. `simulation-and-determinism`: "every tick in validation builds,
/// periodically in shipping lockstep, and on demand".
enum class HashFrequency : u8 {
    Never = 0,
    EveryTick,
    Periodic,
    OnDemand,
};

struct HashSchedule {
    HashFrequency frequency = HashFrequency::Never;
    /// `Periodic` only: hash when `tick % period == 0`. A period of zero is rejected rather than
    /// treated as every tick, because the two are different intentions.
    u32 period = 60;

    [[nodiscard]] constexpr bool due(u64 tick) const noexcept {
        switch (frequency) {
            case HashFrequency::Never:
            case HashFrequency::OnDemand:
                return false;
            case HashFrequency::EveryTick:
                return true;
            case HashFrequency::Periodic:
                return period != 0 && (tick % period) == 0;
        }
        return false;
    }
};

}  // namespace cy::determinism
