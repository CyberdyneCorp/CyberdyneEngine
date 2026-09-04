#pragma once
// The components a node is made of, and the per-world ids they are registered under. Tasks 3.1.1,
// 3.1.3, 3.1.5.
//
// `scene-graph-and-nodes` — "Node is a view onto an entity": a node "SHALL NOT duplicate component
// data", and reading or writing a node property "SHALL read or write the underlying component".
// This header is what that sentence resolves to: every property `Node` exposes is one of the
// structs below, living in a chunk, owned by the ECS. There is no node object holding a copy.
//
// THE AUTHORED/DERIVED SPLIT IS THE ONE STRUCTURAL RULE HERE, AND IT IS REPEATED THREE TIMES.
//
//   LocalTransform  authored          WorldTransform  derived by propagation
//   NodeFlags       authored          Hidden/Disabled derived by propagation
//   ChildOrder      authored          the ECS `Children` buffer is the ECS's own view of the edge
//
// A derived component is not a cache of an authored one: it is a different value, computed from the
// authored one *and the parent chain*, by exactly one system, at exactly one point in the frame.
// The distinction matters because a cache may be stale and still be read; a derived component that
// is stale is a coherence violation, and coherence.h is the test that says so.
//
// WHY EFFECTIVE VISIBILITY IS A TAG AND NOT A BOOL. `scene-graph-and-nodes` requires that a
// disabled subtree be excluded from "queries used by gameplay systems". A bool field cannot do
// that: a query would still visit every row and test it. A tag is part of the archetype, so
// `without(Disabled)` excludes the chunk before it is touched — which is the whole reason the ECS
// has the kind. The cost is an archetype transition per node whose effective state changes, paid at
// the stage flush; the alternative is paying a branch per node per system per frame forever.
//
// WHY THESE ARE BUILT-IN COMPONENTS RATHER THAN REFLECTED ONES. Exactly the seam `src/ecs/`
// records for `Parent` and `Children`: the reflection generator's annotated-header list lives in
// src/core/reflect/CMakeLists.txt and the identifiers in identity/manifest.toml, neither of which
// src/scene/ owns this milestone. Fabricating manifest identifiers here would be inventing the one
// kind of number `core-type-system` says must be assigned once and never guessed. They are
// registered by name instead — the route `ComponentRegistry::register_builtin` exists for — and a
// serialized world names them, which the ECS byte stream already supports. Moving them to reflected
// registration is a change to these two registration calls and to nothing that consumes them.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/values/name.h>
#include <cy/ecs/world.h>

namespace cy::scene {

using ecs::ComponentTypeId;
using ecs::Entity;
using ecs::kInvalidComponent;
using ecs::World;

/// A node's name, unique among its siblings. Present on every node and on nothing else, which is
/// what makes "this entity has a node" a mask test rather than a side table — and what makes
/// `scene-graph-and-nodes`' second coherence invariant ("every entity with a node has exactly one
/// node") true by construction rather than by maintenance.
struct NodeName {
    Name value;
};

/// A project-unique alias, for the stable references from script the specification asks for.
/// Optional: most nodes have none.
struct NodeAlias {
    Name value;
};

/// The node's position among its siblings.
///
/// `ecs-core` leaves the order of the `Children` buffer unspecified, and it means it: removing a
/// child swaps the last one into the gap. `scene-graph-and-nodes` requires ordered children, so the
/// authored order is stored where every other authored property is — in a component — and the
/// buffer stays the ECS's unordered view of the same edge.
struct ChildOrder {
    u32 value = 0;
};

/// The authored placement, relative to the parent. The value a designer edits and a prefab stores.
struct LocalTransform {
    Transform value;
};

/// The derived placement, in world space. Computed by transform propagation from `LocalTransform`
/// and the parent chain; a root's is equal to its `LocalTransform`, which propagation.cpp asserts
/// rather than assumes.
struct WorldTransform {
    Transform value;
};

/// The previous tick's `WorldTransform`, for a node marked interpolatable. Present only on those
/// nodes: `scene-graph-and-nodes` makes interpolation opt-in, and a node that does not opt in must
/// not pay 40 bytes a row for it.
struct InterpolatedTransform {
    Transform previous;
    /// Set by `Node::teleport()`, cleared by the next simulation-phase propagation. While it is
    /// set, `render_transform()` returns the current world transform rather than a blend — which is
    /// the "teleport flag SHALL suppress interpolation for that frame" scenario.
    bool teleport = false;
};

/// The two authored flags. Orthogonal in meaning — one is about rendering and one about simulation
/// — and in one component because they are written together, inherited together, and propagated in
/// the same pass.
struct NodeFlags {
    bool visible = true;
    bool enabled = true;
};

/// Propagation bookkeeping: which of this node's derived values are out of date, and whether
/// anything below it is.
///
/// `scene-graph-and-nodes` words the transform rule as "skipping subtrees whose `LocalTransform`
/// version has not changed". A stored version would still cost a visit per node to compare, so the
/// dirty state is pushed *up* at write time instead: marking a node walks to the root setting
/// `kSubtreeTransform`, and propagation stops descending the instant it meets a node with neither
/// bit. That is O(depth) on a write and O(depth + subtree) on a propagation, where the version
/// compare is O(nodes) — and it is what makes the specification's "only that node's subtree SHALL
/// be recomputed" measurable rather than aspirational.
struct NodeState {
    u8 dirty = 0;
};

/// This node's own transform is out of date.
inline constexpr u8 kDirtyTransform = 1U << 0U;
/// Something at or below this node has `kDirtyTransform`. The bit that makes a skip O(1).
inline constexpr u8 kDirtySubtreeTransform = 1U << 1U;
/// This node's effective visibility or enablement is out of date.
inline constexpr u8 kDirtyFlags = 1U << 2U;
inline constexpr u8 kDirtySubtreeFlags = 1U << 3U;
/// What a freshly created node carries: everything derived from it has never been computed.
inline constexpr u8 kDirtyAll =
    kDirtyTransform | kDirtySubtreeTransform | kDirtyFlags | kDirtySubtreeFlags;

/// Which scene a node was loaded with, so unloading that scene destroys exactly its entities.
/// Zero means "not part of any loaded scene" — a node created directly by code.
struct SceneRef {
    u32 scene = 0;
};

/// The behaviour instance attached to this node, as an index into the behaviour registry's pool.
/// Optional: `scene-graph-and-nodes` makes a behaviour "an optional behaviour", and the
/// overwhelming majority of nodes in a scene have none.
struct BehaviourRef {
    u32 instance = 0;
};

/// The name each component is registered under. Public because a serializer binds a stream to a
/// world by these names — the ECS byte stream's name route — and because a test that asserts on a
/// registration should not be spelling a string literal a second time.
inline constexpr const char* kNodeNameComponentName = "cy::scene::NodeName";
inline constexpr const char* kNodeAliasComponentName = "cy::scene::NodeAlias";
inline constexpr const char* kChildOrderComponentName = "cy::scene::ChildOrder";
inline constexpr const char* kLocalTransformComponentName = "cy::scene::LocalTransform";
inline constexpr const char* kWorldTransformComponentName = "cy::scene::WorldTransform";
inline constexpr const char* kInterpolatedTransformComponentName =
    "cy::scene::InterpolatedTransform";
inline constexpr const char* kNodeFlagsComponentName = "cy::scene::NodeFlags";
inline constexpr const char* kNodeStateComponentName = "cy::scene::NodeState";
inline constexpr const char* kSceneRefComponentName = "cy::scene::SceneRef";
inline constexpr const char* kBehaviourRefComponentName = "cy::scene::BehaviourRef";
inline constexpr const char* kHiddenComponentName = "cy::scene::Hidden";
inline constexpr const char* kDisabledComponentName = "cy::scene::Disabled";

/// The scene layer's component ids in one world.
///
/// Ids are per world (`ecs-core`), so this is a value a `SceneTree` holds and a `Node` reaches
/// through — never a static. Two worlds in one process assign different numbers to `LocalTransform`
/// and neither is wrong.
struct SceneComponents {
    ComponentTypeId node_name = kInvalidComponent;
    ComponentTypeId node_alias = kInvalidComponent;
    ComponentTypeId child_order = kInvalidComponent;
    ComponentTypeId local_transform = kInvalidComponent;
    ComponentTypeId world_transform = kInvalidComponent;
    ComponentTypeId interpolated_transform = kInvalidComponent;
    ComponentTypeId flags = kInvalidComponent;
    ComponentTypeId state = kInvalidComponent;
    ComponentTypeId scene_ref = kInvalidComponent;
    ComponentTypeId behaviour_ref = kInvalidComponent;
    /// Effective visibility, as a tag so a render query can exclude a whole chunk.
    ComponentTypeId hidden = kInvalidComponent;
    /// Effective enablement, as a tag so a gameplay query can exclude a whole chunk.
    ComponentTypeId disabled = kInvalidComponent;

    /// Register all of them in `world`, in this order, and return the ids.
    ///
    /// Idempotent: `ComponentRegistry::register_builtin` returns the existing id for a name it has
    /// already seen, so a second `SceneTree` over one world binds to the same numbers rather than
    /// registering a second set.
    [[nodiscard]] static Expected<SceneComponents, Error> register_all(World& world) noexcept;
};

}  // namespace cy::scene
