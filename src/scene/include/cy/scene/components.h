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
// SEVEN OF THESE TWELVE ARE REFLECTED, AND FIVE ARE NOT. M5's task 1.3, and the seam M2 recorded
// here is now half closed — so this note says exactly where the line falls and why, because "some
// of them" is the kind of statement that rots.
//
// WHAT CHANGED. M2 left every one of these registered by name with no `TypeInfo` behind it, and
// gave the honest reason: the generator's annotated-header list and the identity manifest were not
// src/scene/'s to edit, and fabricating a manifest identifier is inventing the one number
// `core-type-system` says must be assigned once and never guessed. Both of those are now normal
// operations — the generator takes a `--module` group so a module's generated code lives in its own
// directory at its own layer, and the manifest assigns the identifiers — so the seven components
// below that can be described are described. The state hash covers them (M2's gap, still open and
// widening until now) and the editor's generated inspector can read and write them with no per-type
// editor code, which is M5's headline requirement.
//
// WHAT IS STILL REGISTERED BY NAME, AND WHY EACH ONE IS RIGHT TO BE:
//
//   NodeName, NodeAlias   hold a `cy::Name`, which is a private 32-bit index into a process-wide
//                         intern table. Its VALUE is not its representation — `Name::from_index`
//                         says so in as many words — so reflecting the index would describe a
//                         number that means nothing outside this process, and an inspector editing
//                         it would be editing a table position. A name crosses as text, which needs
//                         a `Var`-shaped field the reflection model does not have yet.
//   InterpolatedTransform its fields are `determinism::Presentation<>`, whose value is behind
//                         `bypass_classification()` rather than in a public member. The generator
//                         flattens MEMBERS; reaching through an accessor would be reaching around
//                         the firewall the wrapper exists to be. It is also derived per frame and
//                         never authored, so an inspector has nothing to do with it.
//   Hidden, Disabled      tags. They have no struct and no bytes: their presence IS the value, and
//                         `world_has_component` is what reads them.
//
// The registration route below therefore has two arms, and `SceneComponents::register_all` names
// which components take which.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/math/transform.h>
#include <cy/core/reflect/annotations.h>
#include <cy/core/values/name.h>
#include <cy/ecs/world.h>

#include <type_traits>

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
struct CY_REFLECT_TYPE(Category("Hierarchy"), Tooltip("A node's position among its siblings"))
    ChildOrder {
    CY_REFLECT_FIELD(Category("Hierarchy"), Persistence(Authoring)) u32 value = 0;
};

/// The authored placement, relative to the parent. The value a designer edits and a prefab stores.
///
/// REFLECTED AS NINE FLOATS, NOT AS ONE TRANSFORM. `cy::Transform` is a `Quat` and two `Vec3`s of
/// public floats, and the generator flattens an annotated aggregate member into its leaves —
/// `value.translation.x` and its eight siblings, each with its own manifest identifier and its own
/// exact `offsetof`. That is what a property grid shows, what a gizmo writes one axis of, and what
/// the state hash can fold; a single opaque field would be none of those.
struct CY_REFLECT_TYPE(Category("Transform"),
                       Tooltip("The authored placement, relative to the parent")) LocalTransform {
    CY_REFLECT_FIELD(Category("Transform"), Persistence(Authoring)) Transform value;
};

/// The derived placement, in world space. Computed by transform propagation from `LocalTransform`
/// and the parent chain; a root's is equal to its `LocalTransform`, which propagation.cpp asserts
/// rather than assumes.
struct CY_REFLECT_TYPE(Category("Transform"), Tooltip("The derived placement, in world space"))
    WorldTransform {
    /// `Derived`, not `Authoring`: propagation computes it every frame from `LocalTransform` and
    /// the parent chain, so it is excluded from serialization and an inspector shows it read-only.
    CY_REFLECT_FIELD(Category("Transform"), Persistence(Derived), ReadOnly) Transform value;
};

/// The previous tick's `WorldTransform`, for a node marked interpolatable. Present only on those
/// nodes: `scene-graph-and-nodes` makes interpolation opt-in, and a node that does not opt in must
/// not pay 40 bytes a row for it.
///
/// THIS IS THE ENGINE'S FIRST CLASSIFIED COMPONENT, AND IT IS THE RIGHT ONE. Task 1.3, carried
/// forward from M2: `determinism::Classified<>` was correct and adopted by nothing, so the
/// determinism firewall guarded zero fields. Both fields here are `Presentation`, which is exactly
/// what they are — this component exists so a renderer can blend between two ticks, and
/// `simulation-and-determinism` names "animation pose, camera, audio, VFX, GPU-produced data,
/// illumination" as the class. Wrapping them means an authoritative system **cannot name the value
/// at all**: `read()` requires a witness and the overload does not exist for a crossing the
/// firewall forbids, so gameplay reading an interpolated position is a compile error rather than a
/// divergence M9's replay has to find.
///
/// The direction that stays open is the one that should: an authoritative system may *write* a
/// presentation field (`Node::teleport()` does), because authority flowing downhill is legal and it
/// is only the read back up that is not.
///
/// Layout is unchanged — `Classified<C, T>` is the same size and alignment as `T` and is trivially
/// copyable when `T` is, both asserted in classification.h — so the chunk this component occupies
/// is the chunk it occupied before, and the ECS still accepts it as trivially relocatable.
///
/// WHY THE OTHER ELEVEN ARE NOT WRAPPED YET, honestly: this one has five use sites and the rest
/// have between six and twenty-four each, and every one of them would have to grow a witness. That
/// is a change to src/scene/'s bodies rather than to its data, and it belongs with whoever is
/// editing those bodies rather than in a pass over a module that is otherwise untouched.
/// `WorldTransform`
/// (`Derived`) and `NodeState` (`Derived`) are the next two, and they are next because a derived
/// value read by an authoritative system is the second-most-likely determinism defect after a
/// presentation one.
struct InterpolatedTransform {
    determinism::Presentation<Transform> previous;
    /// Set by `Node::teleport()`, cleared by the next simulation-phase propagation. While it is
    /// set, `render_transform()` returns the current world transform rather than a blend — which is
    /// the "teleport flag SHALL suppress interpolation for that frame" scenario.
    determinism::Presentation<bool> teleport;
};

static_assert(sizeof(InterpolatedTransform) == sizeof(Transform) + alignof(Transform),
              "Classified<> must not change this component's layout: the chunk's column width is "
              "computed from it");
static_assert(std::is_trivially_copyable_v<InterpolatedTransform>,
              "the ECS refuses a component that is not trivially relocatable");

/// The two authored flags. Orthogonal in meaning — one is about rendering and one about simulation
/// — and in one component because they are written together, inherited together, and propagated in
/// the same pass.
struct CY_REFLECT_TYPE(Category("Node"), Tooltip("The two authored flags")) NodeFlags {
    CY_REFLECT_FIELD(Category("Node"), Persistence(Authoring)) bool visible = true;
    CY_REFLECT_FIELD(Category("Node"), Persistence(Authoring)) bool enabled = true;
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
struct CY_REFLECT_TYPE(Category("Node"), Tooltip("Which derived values are out of date"))
    NodeState {
    /// `Derived` and `Hidden`: propagation owns it, it is meaningless in a saved scene, and an
    /// inspector that offered it would be offering a way to corrupt the propagation pass.
    CY_REFLECT_FIELD(Flags(None = 0, Transform = 1, SubtreeTransform = 2, Flags = 4,
                           SubtreeFlags = 8),
                     Category("Node"), Persistence(Derived), Hidden)
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
struct CY_REFLECT_TYPE(Category("Scene"), Tooltip("Which loaded scene this node came with"))
    SceneRef {
    CY_REFLECT_FIELD(Category("Scene"), Persistence(RuntimeState), ReadOnly) u32 scene = 0;
};

/// The behaviour instance attached to this node, as an index into the behaviour registry's pool.
/// Optional: `scene-graph-and-nodes` makes a behaviour "an optional behaviour", and the
/// overwhelming majority of nodes in a scene have none.
struct CY_REFLECT_TYPE(Category("Behaviour"), Tooltip("The behaviour instance attached here"))
    BehaviourRef {
    /// `RuntimeState`, because it is a pool index rather than an authored value: what a prefab
    /// stores is which behaviour TYPE to attach, and the instance is made when the node is spawned.
    CY_REFLECT_FIELD(Category("Behaviour"), Persistence(RuntimeState), ReadOnly) u32 instance = 0;
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
