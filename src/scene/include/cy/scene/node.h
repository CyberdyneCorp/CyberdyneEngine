#pragma once
// `Node` — a named, hierarchical handle onto an entity. Tasks 3.1.1, 3.1.2, 3.1.3, 3.1.5.
//
// `scene-graph-and-nodes` — "Node is a view onto an entity": "a lightweight handle carrying an
// `Entity` plus tree bookkeeping (name, parent, child order). It SHALL NOT duplicate component
// data. Reading or writing a node property SHALL read or write the underlying component."
//
// WHAT A NODE IS, EXACTLY. Two words: a `SceneTree*` and an `Entity`. Nothing else, and the
// `static_assert` at the bottom of this header is what keeps it that way — a later agent who adds a
// cached transform, a dirty flag or a name copy fails the build on that line rather than in a
// debugging session six months later about which of the two values was right.
//
// The tree pointer is not state either: it is the world reference the specification names, plus the
// component ids, which are per world (`ecs-core`) and so cannot be static. Every accessor below is
// a lookup into the ECS through it.
//
// A NODE IS A VALUE, NOT AN OWNER. Copying one costs sixteen bytes and creates no relationship;
// destroying one destroys nothing. `destroy()` destroys the *entity*, and every other copy of the
// node then reports `valid() == false`, because liveness is the entity table's answer and not a
// flag the node carries. That is the specification's "node destroyed with entity" scenario, and it
// needs no invalidation pass to hold.
//
// NAMING. A name is unique among siblings, and a collision is resolved by suffixing (`Light`,
// `Light_2`, `Light_3`). Suffixing rather than refusal is what the specification asks for, and it
// is what makes loading the same prefab twice under one parent work without the caller inventing
// names.
//
// PATHS. `find()` accepts an absolute path (`/Level/Player`, whose first segment names a root),
// a relative path (`Turret/Muzzle`), `..` for the parent and `.` for this node. Resolution is a
// walk of the ECS relation, never a lookup in a table the scene layer maintains — so a path cannot
// resolve to something a query would disagree with.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/scene/components.h>

#include <string_view>
#include <type_traits>

namespace cy::scene {

class SceneTree;

/// What happens to a node's local transform when its parent changes.
enum class Reparent : u8 {
    /// The local transform is kept, so the node moves with its new parent's frame. The cheap one.
    KeepLocal = 0,
    /// The local transform is recomputed from the new parent's inverse so the world transform is
    /// unchanged. `scene-graph-and-nodes`' "reparenting preserves world transform".
    KeepWorld = 1,
};

/// A handle onto one entity's node-shaped view of itself.
class Node {
public:
    constexpr Node() noexcept = default;
    Node(SceneTree& tree, Entity entity) noexcept : tree_(&tree), entity_(entity) {}

    /// True when this handle names a live entity that still has a node. False for a default handle,
    /// for a destroyed entity, and for an entity whose `NodeName` was removed.
    ///
    /// The null test is inline and the rest is not, deliberately: every accessor below is guarded
    /// by `valid()` and then dereferences the tree, and only an inlined null test lets the compiler
    /// see that the dereference is safe. Without it `-Wnull-dereference`, which this tree builds
    /// with as an error, is right to complain.
    [[nodiscard]] bool valid() const noexcept { return tree_ != nullptr && alive_with_node(); }

    [[nodiscard]] constexpr Entity entity() const noexcept { return entity_; }
    [[nodiscard]] constexpr SceneTree* tree() const noexcept { return tree_; }
    [[nodiscard]] World* world() const noexcept;

    friend constexpr bool operator==(Node, Node) noexcept = default;

    // --- Naming and identity (task 3.1.2) -------------------------------------------------------

    [[nodiscard]] Name name() const noexcept;
    /// Set the name, suffixing if a sibling already has it. The name actually taken is readable
    /// back from `name()`, which is not necessarily the one passed.
    [[nodiscard]] Status set_name(Name name) const noexcept;

    [[nodiscard]] Name alias() const noexcept;
    /// Claim a project-unique alias. Refuses `AlreadyExists` when another live node holds it.
    [[nodiscard]] Status set_alias(Name alias) const noexcept;
    [[nodiscard]] Status clear_alias() const noexcept;

    /// The absolute path of this node, NUL-terminated in `out`. `out` is cleared first.
    [[nodiscard]] Status path(Array<char>& out) const noexcept;

    // --- Hierarchy (task 3.1.2) -----------------------------------------------------------------

    [[nodiscard]] Node parent() const noexcept;
    [[nodiscard]] u32 child_count() const noexcept;
    /// The `index`-th child in authored order, or a null node.
    [[nodiscard]] Node child(u32 index) const noexcept;
    /// The child with this name, or a null node.
    [[nodiscard]] Node child(Name name) const noexcept;
    /// Every child, in authored order. Appends to `out` without clearing it.
    [[nodiscard]] Status children(Array<Node>& out) const noexcept;
    [[nodiscard]] u32 depth() const noexcept;
    [[nodiscard]] bool is_ancestor_of(Node other) const noexcept;

    /// Resolve a path from this node. See the header comment for the accepted forms.
    [[nodiscard]] Node find(std::string_view path) const noexcept;

    /// Make `parent` this node's parent, or detach when `parent` is null. Structural: it is refused
    /// while a query iterates, exactly as the underlying `World::set_parent` is.
    [[nodiscard]] Status set_parent(Node parent,
                                    Reparent mode = Reparent::KeepLocal) const noexcept;
    [[nodiscard]] Status add_child(Node child, Reparent mode = Reparent::KeepLocal) const noexcept;

    /// Move this node to `index` among its siblings, shifting the others. Authored order only; the
    /// ECS `Children` buffer is unaffected because its order is unspecified by `ecs-core`.
    [[nodiscard]] Status set_sibling_index(u32 index) const noexcept;
    [[nodiscard]] u32 sibling_index() const noexcept;

    // --- Transforms (task 3.1.3) ----------------------------------------------------------------

    [[nodiscard]] Transform local_transform() const noexcept;
    /// Write the authored placement and mark the subtree for propagation.
    [[nodiscard]] Status set_local_transform(const Transform& value) const noexcept;

    /// The derived placement as of the last propagation. Reading it does not propagate: a value
    /// that recomputed itself on read would make the cost of a read unpredictable and would hide a
    /// missing propagation system rather than exposing it.
    [[nodiscard]] Transform world_transform() const noexcept;

    /// Assign a world placement. The `LocalTransform` is derived from the parent's inverse and
    /// written, keeping the authored value authoritative — `scene-graph-and-nodes`' "writing world
    /// transform" scenario. The `WorldTransform` component is *not* written here; propagation
    /// writes it, which is what keeps one system responsible for it.
    [[nodiscard]] Status set_world_transform(const Transform& value) const noexcept;

    /// Tell propagation this node's `LocalTransform` was written by something other than the node
    /// API — a system writing the column directly. The supported spelling; without it the write is
    /// invisible to propagation and `check_coherence()` reports it.
    [[nodiscard]] Status mark_transform_changed() const noexcept;

    [[nodiscard]] bool interpolated() const noexcept;
    /// Opt this node into render interpolation, which costs it an `InterpolatedTransform` column.
    [[nodiscard]] Status set_interpolated(bool interpolated) const noexcept;
    /// Suppress interpolation until the next simulation tick. What a teleport calls.
    [[nodiscard]] Status teleport() const noexcept;
    /// The transform to render at `alpha` between the previous tick and this one. Equal to
    /// `world_transform()` for a node that is not interpolated or has teleported.
    [[nodiscard]] Transform render_transform(f32 alpha) const noexcept;

    // --- Visibility and enablement (task 3.1.5) -------------------------------------------------

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] Status set_visible(bool visible) const noexcept;
    [[nodiscard]] Status set_enabled(bool enabled) const noexcept;

    /// The inherited answer, as of the last propagation. Read off the `Hidden`/`Disabled` tags,
    /// which *are* the effective state rather than a copy of it — see components.h.
    [[nodiscard]] bool effective_visible() const noexcept;
    [[nodiscard]] bool effective_enabled() const noexcept;

    /// True once the node is reachable from the tree root. What `onEnterTree` is the transition of.
    [[nodiscard]] bool in_tree() const noexcept;

    // --- Components (task 3.1.4) ----------------------------------------------------------------
    //
    // EVERY MUTATOR ON A `Node` IS `const`, AND THAT IS THE TYPE TELLING THE TRUTH. A node is a
    // handle: writing through it changes the *world*, never the handle, exactly as writing through
    // a `T* const` changes the pointee. A non-const mutator would suggest the node holds the value
    // being written — which is the one thing this class must not do.
    //
    // The escape hatch that makes composition work: a node is its entity, so anything the ECS can
    // hold, a node has. These forward to the world unchanged, and are here so that script and
    // authoring code do not have to reach past the façade to reach the data the façade is over.

    [[nodiscard]] bool has(ComponentTypeId component) const noexcept;
    [[nodiscard]] const void* get(ComponentTypeId component) const noexcept;
    [[nodiscard]] void* get_mut(ComponentTypeId component) const noexcept;
    [[nodiscard]] Status add(ComponentTypeId component, const void* value = nullptr) const noexcept;
    [[nodiscard]] Status remove(ComponentTypeId component) const noexcept;

    template <class T>
    [[nodiscard]] const T* get_as(ComponentTypeId component) const noexcept {
        return static_cast<const T*>(get(component));
    }
    template <class T>
    [[nodiscard]] Status set(ComponentTypeId component, const T& value) const noexcept {
        void* slot = get_mut(component);
        if (slot == nullptr) {
            return fail(ErrorCode::NotFound, "this node does not have that component");
        }
        *static_cast<T*>(slot) = value;
        return ok();
    }

    // --- Groups (task 3.1.8) --------------------------------------------------------------------

    [[nodiscard]] Status add_to_group(Name group) const noexcept;
    [[nodiscard]] Status remove_from_group(Name group) const noexcept;
    [[nodiscard]] bool in_group(Name group) const noexcept;

    // --- Lifetime -------------------------------------------------------------------------------

    /// Destroy this node's entity and its whole subtree, firing `onExitTree` and `onDestroy`
    /// child-first before any component is released.
    [[nodiscard]] Status destroy() const noexcept;

private:
    /// The entity is alive and still carries a `NodeName`. The out-of-line half of `valid()`.
    [[nodiscard]] bool alive_with_node() const noexcept;

    [[nodiscard]] Node resolve_segment(std::string_view segment) const noexcept;

    SceneTree* tree_ = nullptr;
    Entity entity_;
};

// THE RULE THIS MODULE EXISTS TO UPHOLD, AS A BUILD ERROR. A node is a pointer and an entity id. It
// holds no transform, no name copy, no dirty flag and no child list; adding one breaks this line
// first. See design.md §3 and tests/test_coherence.cpp.
static_assert(sizeof(Node) == sizeof(void*) + sizeof(Entity),
              "a Node is a tree pointer and an entity id — nothing else. See design.md §3.");
static_assert(std::is_trivially_copyable_v<Node>,
              "a Node is a value: copying one must not create a relationship");

}  // namespace cy::scene
