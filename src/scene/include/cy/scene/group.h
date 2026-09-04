#pragma once
// Groups: named sets of nodes, iterable and broadcastable. Task 3.1.8.
//
// `scene-graph-and-nodes` — "Groups and tags": nodes are assignable to named groups, the engine
// provides "efficient iteration over a group and broadcast invocation across it", and — the
// sentence that decides the implementation — "Groups SHALL be implemented as tag components so
// group membership is queryable from systems."
//
// So a group is not a list the scene layer keeps. It is a zero-sized component registered under the
// name `cy::scene::group/<name>`, and membership is a bit in the entity's archetype mask. A system
// that wants "every enemy" writes `without`/`with` against that id and never learns that a scene
// tree exists; a designer who tags a node in the editor is adding a component. One representation,
// queryable from both ends.
//
// WHAT THIS COSTS, SAID OUT LOUD. A world registers at most `ecs::kMaxComponentTypes` (256)
// component types, and every group consumes one of them permanently — the ECS never unregisters a
// component, because an id is an index into tables an archetype already holds. A project with
// hundreds of dynamic group names will exhaust the world, and it will do so with
// `ErrorCode::OutOfRange` naming the limit rather than by degrading. Groups are for the tens of
// named sets a game actually broadcasts to, which is what the specification's example ("enemies")
// describes; a per-entity attribute with thousands of values is a component with a field, not a
// group.
//
// BROADCAST ORDER IS SORTED BY ENTITY INDEX, NOT BY CHUNK ORDER. The specification says "in a
// deterministic order". Chunk order is deterministic within one run but depends on the order
// entities happened to be created and moved, so two runs that build the same scene by different
// routes would broadcast in different orders. The entity index is the world's own dense counter and
// gives an order that is reproducible across runs, which is the property M9's determinism work will
// need.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/scene/components.h>

namespace cy::scene {

class Node;
class SceneTree;

/// The prefix every group's tag component name carries, so a group is recognisable in a component
/// listing and cannot collide with an engine or game component.
inline constexpr const char* kGroupComponentPrefix = "cy::scene::group/";

/// What a broadcast calls, once per member.
using GroupVisitor = void (*)(Node node, void* user) noexcept;

/// One world's groups.
class GroupRegistry {
public:
    explicit GroupRegistry(Allocator& allocator) noexcept
        : allocator_(&allocator), names_(allocator), ids_(allocator) {}

    /// The tag component for `group`, registering it if this is the first time it is named.
    [[nodiscard]] Expected<ComponentTypeId, Error> ensure(World& world, Name group) noexcept;

    /// The tag component for `group`, or `kInvalidComponent` when nothing has ever joined it.
    /// Never registers, so a membership *test* cannot grow the world's component table.
    [[nodiscard]] ComponentTypeId find(Name group) const noexcept;

    /// Every live member of `group`, sorted by entity index. Appends to `out`.
    [[nodiscard]] Status members(SceneTree& tree, Name group, Array<Node>& out) const noexcept;

    /// Call `visitor` for every member, in the same order `members()` produces.
    ///
    /// The visitor runs *outside* the query that collected the members, so it may make structural
    /// changes: collecting first costs one array and buys a broadcast whose body is allowed to
    /// destroy the node it was handed, which is what a broadcast is usually for.
    [[nodiscard]] Status broadcast(SceneTree& tree, Name group, GroupVisitor visitor,
                                   void* user) const noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(names_.size()); }
    /// The groups, in the order they were first named. Appends to `out`.
    [[nodiscard]] Status names(Array<Name>& out) const noexcept;

private:
    Allocator* allocator_;
    Array<Name> names_;
    /// Group name index to component id. `Name` is already a dense counter, so it is its own hash.
    HashMap<u32, ComponentTypeId> ids_;
};

}  // namespace cy::scene
