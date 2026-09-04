#pragma once
// The state schema for the ECS's own built-in components. Task 1.2, carried forward from M2.
//
// `simulation-and-determinism` requires the state hash to cover authoritative state. M2 shipped a
// hash covering **4 of 17 subjects**: `declare_reflected_components()` declares every component
// that carries a `reflect::TypeInfo`, and the ECS's `Parent`/`Children` and all twelve of the
// scene's built-ins carry none — they are registered by name (component.h's second route), so the
// walk counted them as undeclared and hashed nothing. The consequence was concrete and is the
// reason this file exists: **a divergence in a node's parent did not change the hash.**
//
// The route is the one `state_schema.h` documents for a component with no descriptor to derive
// from: an explicit field list, with offsets taken from the struct and each field's class stated.
// `src/scene/state_schema.h` does the same for the scene's twelve.
//
// WHY NOT REFLECT THEM INSTEAD, which is the other way to close this. Because reflection cannot
// carry these fields today: a reflected field is a fixed-width scalar, a bool or an enum
// (tools/gen/reflect/parse.py refuses anything else), and `Parent` holds an `Entity` while the
// scene's built-ins hold `Transform`s and `Name`s. Reflecting them means extending `FieldKind` to
// nested aggregates, extending the generator, and issuing manifest identifiers — a change to
// `core-type-system`'s surface, not a registration. An explicit declaration closes the hash gap now
// and stays correct afterwards: when those types are reflected, `declare_reflected()` covers them
// and these functions become the thing that is deleted.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/ecs/component.h>

namespace cy::ecs {

/// Declare a schema for `Parent` and `Children`.
///
/// `parent` and `children` are the world's ids for them — `World::parent_component()` and
/// `World::children_component()`. They are bound into `determinism::SchemaSubject` here, which is
/// the same binding `runtime::subject_of()` makes; layer 0 cannot name a `ComponentTypeId` and the
/// runtime is layer 5, so the two spellings of "a subject is a component" meet in the number rather
/// than in a shared function.
///
/// WHAT IS HASHED, AND WHAT DELIBERATELY IS NOT:
///
///   Parent    the parent's entity **index**, and not its generation. The generation is recycling
///             history: two runs that destroyed and recreated an entity in the same order agree
///             about it, and a run that reached the same state another way does not.
///             `runtime::hash_world` makes exactly this choice for the entity node's own id, and a
///             reference to an entity has to be hashed the same way the entity is or the two
///             disagree about what identity means.
///   Children  DECLARED, WITH NO HASHED FIELDS, ON PURPOSE. `ecs-core` leaves the order of the
///             children buffer unspecified and means it — removing a child swaps the last one into
///             the gap — so the buffer's contents are allocator and operation history, which is the
///             one thing `simulation-and-determinism` says a hash must not depend on. The edge is
///             already covered from the other side by `Parent`, and the authored order is
///             `scene::ChildOrder`, which is hashed. Declaring it with nothing rather than leaving
///             it undeclared is the point: `WorldHashReport` then reports zero undeclared subjects,
///             which is a statement that every subject was considered rather than an omission that
///             looks the same as a coverage gap.
[[nodiscard]] Status declare_relationship_state(determinism::StateSchema& schema,
                                                ComponentTypeId parent,
                                                ComponentTypeId children) noexcept;

}  // namespace cy::ecs
