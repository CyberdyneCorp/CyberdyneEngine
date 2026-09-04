#pragma once
// Transform, visibility and enablement propagation. Tasks 3.1.3 and 3.1.5.
//
// `scene-graph-and-nodes` — "Transform model": propagation "SHALL run as a system in
// `PostSimulation` and again before rendering, processing the hierarchy in depth order and skipping
// subtrees whose `LocalTransform` version has not changed"; and "Visibility and enablement":
// "Effective state SHALL be computed by a propagation system alongside transforms."
//
// ONE WALK, BOTH JOBS. There is one depth-first walk from the roots and it carries both answers
// down, because both are inherited along the same edges and a second walk would double the cost of
// the one traversal in the frame that touches every node. The two have independent dirty bits
// (components.h), so a frame in which only a flag changed does not recompute a transform and a
// frame in which only a transform moved does not touch a tag.
//
// WHY THE WALK IS NOT A QUERY. A query iterates chunks, which is archetype order — the wrong order
// for a hierarchy, where a child's answer needs its parent's. The roots are found with a query
// (`NodeName` without `Parent`), and everything after that is a walk of the ECS relation. It is an
// explicit stack rather than recursion: an authored hierarchy has no depth limit a scene file
// cannot exceed, and a stack overflow in a propagation system is not a failure anyone can diagnose.
//
// THE STRUCTURAL HALF GOES THROUGH A COMMAND BUFFER. Effective visibility and enablement *are* the
// `Hidden` and `Disabled` tags (components.h), and adding a tag is a structural change. Inside a
// stage that is recorded and applied at the stage's flush, which is the same commit point every
// other deferred change lands at; outside one — a test, a tool — the tags are applied directly,
// because there is no iteration in progress to protect.
//
// COST, MEASURED RATHER THAN CLAIMED. A propagation visits `depth + subtree size` nodes per moved
// node, not the whole tree, and `PropagationStats` reports exactly that so the claim is a test
// rather than a comment. Reading a node's components is `World::get_mut`, which the ECS documents
// as a table lookup and a binary search — appropriate here, where the node count is the changed
// set, and the thing to revisit first if a frame ever propagates a whole large hierarchy.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/ecs/command_buffer.h>
#include <cy/ecs/entity.h>
#include <cy/ecs/system.h>

namespace cy::scene {

using ecs::Entity;

class SceneTree;

/// Which of the two propagations this is.
enum class PropagationPhase : u8 {
    /// The one in `PostSimulation`. It also rolls `InterpolatedTransform::previous` forward and
    /// clears teleport flags, because those are per *tick* and not per frame.
    Simulation = 0,
    /// The one before rendering. Transforms and flags only.
    Render = 1,
};

/// What one propagation did. The evidence for "only that node's subtree SHALL be recomputed".
struct PropagationStats {
    u32 roots = 0;
    /// Nodes the walk actually looked at. For one moved node in a large tree this is its depth
    /// plus its subtree, and the difference from `roots`-plus-everything is the whole point.
    u64 nodes_visited = 0;
    u64 transforms_recomputed = 0;
    u64 flags_recomputed = 0;
    /// Subtrees the walk declined to enter because neither dirty bit was set below them.
    u64 subtrees_skipped = 0;
    u64 tags_added = 0;
    u64 tags_removed = 0;
};

/// Mark a node's transform as changed, and its ancestors as containing a change.
///
/// The one function that makes a subtree skip possible, and the supported spelling for a system
/// that writes `LocalTransform` columns directly. O(depth), and it stops at the first ancestor that
/// already knows.
[[nodiscard]] Status mark_transform_changed(SceneTree& tree, Entity entity) noexcept;

/// The same for the two flags.
[[nodiscard]] Status mark_flags_changed(SceneTree& tree, Entity entity) noexcept;

/// Walk the tree and bring every derived value up to date.
///
/// `commands` may be null; see the header comment. `out` may be null.
[[nodiscard]] Status propagate(SceneTree& tree, PropagationPhase phase,
                               ecs::CommandBuffer* commands, PropagationStats* out) noexcept;

/// Register both propagation systems: the `PostSimulation` one and the pre-render one in `Frame`.
///
/// The names are `scene.propagate_simulation` and `scene.propagate_render`; a caller that needs to
/// order its own systems around them names those.
[[nodiscard]] Status install_propagation_systems(SceneTree& tree, ecs::Schedule& schedule) noexcept;

inline constexpr const char* kSimulationPropagationSystem = "scene.propagate_simulation";
inline constexpr const char* kRenderPropagationSystem = "scene.propagate_render";

}  // namespace cy::scene
