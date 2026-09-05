#pragma once
// The physics server's object families, as generational handles. Task 4.2.1.
//
// `physics` — "Engine-owned physics interface": `PhysicsServer` "SHALL be an engine-defined,
// handle-based interface that backends implement", covering "worlds, bodies (static, kinematic,
// dynamic), colliders and shapes, constraints, character controllers, queries, and simulation
// stepping". One handle family per noun in that list, declared together so that a family added
// later does not arrive with a different spelling than the ones already in use.
//
// M1's `cy::Handle<Tag>` exactly: a 32-bit slot index and a 32-bit generation, where freeing a slot
// bumps its generation so a handle held across the free compares unequal to whatever replaced it.
// Every family is a distinct type, so passing a `ShapeHandle` where a `BodyHandle` is expected is a
// compile error rather than a convention.
//
// A null handle is a zero generation, so a zeroed struct — a component in memset chunk storage, a
// designated initialiser that omits the field — reads as "no body" rather than "body 0". That is
// what lets `RigidBody{}` be a valid component before the body behind it exists.

#include <cy/core/values/handle.h>

namespace cy::physics {

CY_HANDLE_TAG(PhysicsWorld);
CY_HANDLE_TAG(PhysicsBody);
CY_HANDLE_TAG(PhysicsShape);
CY_HANDLE_TAG(PhysicsMaterial);
CY_HANDLE_TAG(PhysicsConstraint);

using WorldHandle = Handle<PhysicsWorldTag>;
using BodyHandle = Handle<PhysicsBodyTag>;
using ShapeHandle = Handle<PhysicsShapeTag>;
using MaterialHandle = Handle<PhysicsMaterialTag>;
using ConstraintHandle = Handle<PhysicsConstraintTag>;

/// What a body carries back to its owner, opaque to every backend.
///
/// THE SERVER NEVER DEREFERENCES IT. `engine-architecture` puts servers at layer 2 with "no
/// knowledge of the ECS world, the scene graph, or scripting", so a body cannot hold an
/// `ecs::Entity` — the type is not reachable from here and, more to the point, a server that could
/// reach into ECS storage would be a layering defect. What a body holds is 64 bits the caller chose
/// the meaning of; the ECS bridge stores an entity's bits in it and reads them back out of a
/// contact event. The server only ever copies it.
using UserData = u64;

}  // namespace cy::physics
