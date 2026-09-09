#pragma once
// The state schema for physics' eight components. M8.a task 4.1.
//
// M2's carried-forward debt, restated by `src/rendering/scene/state_schema.h` and paid here in the
// same change that registers the components: **a component registered by name is invisible to the
// state hash unless something declares a schema for it.** Registering `RigidBody` without this file
// would mean a body's mass, its damping, its axis locks and its collider's dimensions could all
// differ between two peers and the state hash would say the worlds agreed.
//
// --- WHAT IS FOLDED, AND WHY EACH ANSWER IS WHAT IT IS -------------------------------------------
//
//   the authored fields        Authoritative   mass, damping, gravity scale, the locks, the shape's
//                                              dimensions, the filter, the material's friction and
//                                              restitution. Every one of them changes what the
//                                              solver computes, so a divergence in any of them is a
//                                              divergence in the world.
//
//   every handle               Derived         `BodyHandle`, `ShapeHandle`, `MaterialHandle`,
//                                              `ConstraintHandle`. A handle's value is the
//                                              backend's slot allocation order — the same argument
//                                              `rendering::MeshHandle` gets, and a stronger one
//                                              here: two peers running two backends would disagree
//                                              on every handle while simulating identically.
//
// --- WHAT IS NOT DECLARED, AND WHY THAT IS NOT A HOLE --------------------------------------------
//
// `ShapeDescription` carries four raw pointers — a convex hull's points, a triangle mesh's vertices
// and indices, a compound's children. **They are addresses, and an address is never state.** The
// geometry behind them is an asset, identified by content, and the hash of a world that references
// it is not the place that identity is established. Folding a pointer would make the hash differ
// between two runs of the same binary on the same machine, which is the one thing a state hash may
// never do. `physics`' own determinism policy says the same thing in different words.
//
// The consequence is stated rather than hidden: a divergence that exists ONLY in a triangle mesh's
// vertex data is not caught by this schema. It is caught by the asset identity that named the mesh.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/physics/components.h>

namespace cy::physics {

/// Declare physics' eight components to `schema`.
///
/// Beside `ecs::declare_relationship_state()`, `scene::declare_scene_state()` and
/// `rendering::declare_render_state()`, before `StateSchema::freeze()`. Refuses when the components
/// are not registered in the world the schema is about, for the reason `declare_render_state`
/// gives: a schema over `kInvalidComponent` addresses nothing while reporting coverage.
[[nodiscard]] Status declare_physics_state(determinism::StateSchema& schema,
                                           const PhysicsComponents& components) noexcept;

}  // namespace cy::physics
