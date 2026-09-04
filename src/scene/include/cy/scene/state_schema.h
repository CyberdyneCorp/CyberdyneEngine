#pragma once
// The state schema for the scene's twelve built-in components. Task 1.2, carried forward from M2.
//
// M2's state hash covered **4 of 17 subjects**. Thirteen of the thirteen missing ones were built-in
// components registered by name with no `reflect::TypeInfo`, so `declare_reflected_components()`
// skipped and counted them and the hash was silent about every one — which meant, concretely, that
// **a divergence in a node's name, its parent, its sibling order or its visibility did not change
// the hash**. `src/ecs/state_schema.h` closes `Parent` and `Children`; this closes the other
// twelve, and the argument for declaring rather than reflecting is there rather than repeated here.
//
// --- WHAT IS HASHED, AND WHY EACH ANSWER IS WHAT IT IS -------------------------------------------
//
// The classification decides participation (`determinism::participation_of`), so this table is the
// hash's coverage read straight off:
//
//   NodeName              Authoritative   the name, AS TEXT — see `StateEncoding::InternedName`.
//   NodeAlias             Authoritative   the same.
//   ChildOrder            Authoritative   the authored sibling order. This is what makes "a
//                                         divergence in sibling order changes the hash" true; the
//                                         ECS's `Children` buffer cannot, because its order is
//                                         unspecified.
//   LocalTransform        Authoritative   ten floats. The authored placement is state.
//   NodeFlags             Authoritative   the two authored flags.
//   SceneRef              Authoritative   which scene the node was loaded with.
//   BehaviourRef          Authoritative   the behaviour instance's pool index.
//
//   WorldTransform        Derived         computed from LocalTransform and the parent chain by one
//                                         system at one point in the frame. `simulation-and-
//                                         determinism` says derived state is recomputed rather than
//                                         hashed, and hashing it would report float-order
//                                         differences in a value both sides can rebuild.
//   NodeState             Derived         propagation's dirty bits: bookkeeping about what has yet
//                                         to be recomputed, which is a property of when the hash
//                                         was taken rather than of the world.
//   InterpolatedTransform Presentation    render history. Classified in components.h, and the
//                                         classification here agrees with the one on the fields —
//                                         a presentation field in the hash would make a render
//                                         blend look like a divergence.
//
//   Hidden, Disabled      Authoritative   TAGS: zero-sized, declared with no fields. Their presence
//                                         is already in the hash through the archetype key, which
//                                         is folded from the component identities of the archetype
//                                         an entity is in — so effective visibility changes the
//                                         hash by moving the entity between archetypes. They are
//                                         declared anyway, for the same reason `Children` is: a
//                                         declared subject with nothing to fold is a statement that
//                                         it was considered, and it is what lets a caller assert
//                                         `subjects_undeclared == 0` and mean something by it.
//
// A subject declared with no hashed fields still costs nothing at hash time: `hash_entity` skips a
// subject whose `hashed_field_count` is zero before it looks the component up.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/scene/components.h>

namespace cy::scene {

/// Declare all twelve, using the ids `SceneComponents::register_all` returned for this world.
///
/// Idempotent it is NOT: `StateSchema::declare` refuses a duplicate subject, which is the right
/// answer — declaring twice means two callers each believe they own the schema.
///
/// Call it after `declare_reflected_components()` and before `freeze()`; with
/// `ecs::declare_relationship_state()` alongside it, a scene world's `WorldHashReport` reports zero
/// undeclared subjects. `tests/integration/test_state_hash_coverage.cpp` is that claim, run.
[[nodiscard]] Status declare_scene_state(determinism::StateSchema& schema,
                                         const SceneComponents& components) noexcept;

}  // namespace cy::scene
