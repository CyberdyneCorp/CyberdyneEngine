#pragma once
// Scenes: a tree of nodes loaded into a world, and unloadable again. Task 3.1.9.
//
// `scene-graph-and-nodes` — "Scenes and worlds": a scene is "a serialized tree of nodes and their
// components, loadable into a world additively or as a replacement"; several may be loaded into one
// world at once, "each tracked so it can be unloaded independently, taking its entities with it";
// and an asynchronous load spreads entity creation "across frames under a budget", the scene
// becoming active "only once fully instantiated".
//
// WHAT THIS HEADER IS AND IS NOT. It is the *runtime* half: the description a loader hands over,
// the identity a loaded scene carries, and the budgeted instantiation. The authoring formats, the
// cook and the reflection-driven traversal that produce a `SceneDescription` are
// `serialization-and- prefabs` in src/scene/serialization/, one module up. The split is deliberate
// and it is the same one design.md §6 draws: this module knows what a node is; that one knows what
// a file is.
//
// MEMBERSHIP IS A COMPONENT, NOT A LIST. Every entity a scene creates carries `SceneRef`, so
// unloading is a query and a destroy rather than a bookkeeping array the scene layer has to keep in
// step with the world. A list would be a second representation of a fact the ECS already holds, and
// the first structural change made outside the scene API would make the two disagree.
//
// A shared component would group a scene's entities into their own chunks and make an unload a
// chunk walk instead of a filtered one. That is the better shape at M6, when a streaming cell is a
// scene; it costs a second structural operation per node at load time, so it is deliberately not
// what M2 does. The change is local to `instantiate_node` and `unload`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/scene/components.h>

namespace cy::scene {

class Node;
class SceneTree;

/// A loaded scene's identity within one tree. Zero is "no scene": an entity created directly by
/// code rather than by a load.
using SceneId = u32;
inline constexpr SceneId kNoScene = 0;

/// One node of a scene, as a loader hands it over.
///
/// `parent` is an index into the same description and must be less than this node's own index, so a
/// description is topologically ordered by construction and instantiation is a single forward pass.
/// `kNoParent` makes the node a child of the scene's root.
struct NodeDesc {
    static constexpr u32 kNoParent = 0xFFFF'FFFFu;

    Name name;
    u32 parent = kNoParent;
    /// The template to instantiate, or empty for a bare spatial node.
    Name node_template;
    Transform local_transform;
    bool visible = true;
    bool enabled = true;
    /// The alias to claim, or empty.
    Name alias;
    /// The behaviour to attach, or empty.
    Name behaviour;
};

/// A scene, ready to instantiate. Borrowed: the array belongs to whoever built it.
struct SceneDescription {
    Name name;
    Span<const NodeDesc> nodes;
};

/// How far a load has got.
enum class SceneStatus : u8 {
    /// Instantiating, under a budget. Its nodes exist but the scene is not active.
    Loading = 0,
    /// Fully instantiated. `scene-graph-and-nodes`' "become active only once fully instantiated".
    Active = 1,
    /// Unloaded; the id is retired and never reissued within this tree.
    Unloaded = 2,
};

const char* scene_status_name(SceneStatus status) noexcept;

/// What one loaded scene is, from outside.
struct SceneInfo {
    SceneId id = kNoScene;
    Name name;
    SceneStatus status = SceneStatus::Loading;
    /// Nodes instantiated so far, and how many the description holds.
    u32 instantiated = 0;
    u32 total = 0;
    Entity root;
};

}  // namespace cy::scene
