#pragma once
// The hierarchical state hash, bound to an ECS world. Task 4.2.6.
//
// `<cy/core/determinism/hash.h>` owns the tree, the mixing and the narrowing, and knows nothing
// about entities — it is layer 0 and an ECS world is layer 1. This file is the walk: it turns a
// `World` into the tree, in an order that is a function of the world's *contents* and not of its
// history, so that two runs which agree about every value produce the same bytes.
//
// --- THE WALK, AND WHY EVERY LEVEL IS ORDERED THE WAY IT IS --------------------------------------
//
//   World       one node, named for the world.
//   Archetype   sorted by a key folded from the archetype's components' STABLE IDENTITIES, sorted
//               before they are folded — NOT by the archetype's own id, which is creation order,
//               and NOT by `ComponentTypeId`, which is registration order.
//               `simulation-and-determinism`: "Determinism SHALL NOT depend on allocator history or
//               archetype creation order by accident", and "registries whose contents affect
//               simulation — systems, types, rules, providers — SHALL be finalised in a
//               deterministic order derived from stable identifiers".
//   Entity      sorted by entity index. Not chunk order: an entity's row is where the allocator put
//               it, and the validator perturbs exactly that. The index is dense, recycled LIFO by a
//               rule the ECS states, and therefore reproducible.
//   Component   in stable-identity order, and the node's id is that identity. NOT the archetype's
//               column order: `ecs-core` fixes columns as sorted *component id*, which is
//               registration order again.
//   Field       in the declared schema's order, and only the fields whose classification
//               participates in hashing.
//
// A COMPONENT'S STABLE IDENTITY is its `reflect::TypeId` — the number `identity/manifest.toml`
// assigns once and never reuses — or, for a built-in registered by name with no descriptor, an
// unseeded FNV-1a of that name. Deliberately not `cy::hash_bytes`, which is seeded per process in
// development builds. Folding the `ComponentTypeId` instead made two worlds of identical content
// hash differently when their components had been registered in a different order; that was
// measured and fixed at M2's close, and `integration.runtime_simulation`'s "the hash does not
// depend on the order components were registered" is the regression. The shipping case it would
// have broken is build-time feature slicing: dropping a module's components shifts every later id.
//
// **There is no Chunk level.** The reason is in `<cy/core/determinism/hash.h>`'s header and is
// worth repeating where the walk is: chunk membership is allocator history, and a hash that
// included it would differ between two runs that agree about everything a game can observe.
//
// --- WHAT IS NOT HASHED, AND WHY THE REPORT SAYS SO ----------------------------------------------
//
// A component with no declared schema contributes nothing and is **counted**. Hashing its bytes
// instead is not an option — `simulation-and-determinism` lists "raw structure memory hashed or
// serialised as canonical authoritative state" among the forbidden patterns — so the honest
// alternative is to say how much of the world the hash does not cover. `WorldHashReport` is that
// statement, and a caller that wants full coverage declares the missing subjects.
//
// At M2 that matters: the ECS's `Parent`/`Children` and all twelve of the scene's built-in
// components are registered by name with no `TypeInfo` behind them (both modules' READMEs record
// the seam), so a scene world's hash covers its *reflected game components* and reports the rest.
// `declare_reflected_components()` below declares everything the world can describe; anything left
// needs `StateSchema::declare()` with an explicit field list.
//
// --- COST ----------------------------------------------------------------------------------------
//
// One full walk: O(entities log entities) for the per-archetype sort, plus one `World::get` per
// (entity, component), which the ECS documents as a table lookup and a binary search. It is not
// incremental and it is not the shape a per-tick shipping hash would use — that wants a per-chunk
// subtree cached against the chunk's version, which is M9's. The runtime measures and reports the
// cost so that the decision to build it is taken against a number.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/provider.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/ecs/world.h>

namespace cy::runtime {

/// What one walk covered, and what it did not.
struct WorldHashReport {
    u32 archetypes_visited = 0;
    u32 entities_hashed = 0;
    u32 components_hashed = 0;
    u32 fields_hashed = 0;
    /// Component types encountered that have a declared schema, and that do not. The second is the
    /// number that says how much of the world the hash is silent about.
    u32 subjects_declared = 0;
    u32 subjects_undeclared = 0;
    /// The root hash, repeated here so a caller that only wants the number does not have to reach
    /// into the tree.
    u64 hash = 0;
};

/// Bind a component type id into a schema subject. One line, in one place, so that "a subject is a
/// component" is a fact with an address rather than an assumption spread over two modules.
[[nodiscard]] constexpr determinism::SchemaSubject subject_of(
    ecs::ComponentTypeId component) noexcept {
    return determinism::SchemaSubject{component};
}

/// Declare a schema for every reflected component the world has registered.
///
/// Each field's class is derived from its `PersistenceKind` (`determinism::class_of`). A component
/// with no `TypeInfo` — every built-in — is skipped and counted; declaring one needs an explicit
/// field list, because there is nothing to derive from.
///
/// Does not freeze the schema: a caller usually adds its own built-in declarations afterwards and
/// freezes once.
struct SchemaDeclarationReport {
    u32 declared = 0;
    u32 already_declared = 0;
    u32 skipped_unreflected = 0;
};

[[nodiscard]] Status declare_reflected_components(const ecs::World& world,
                                                  determinism::StateSchema& schema,
                                                  SchemaDeclarationReport& report) noexcept;

/// Walk `world` into `tree`.
///
/// `tree` is cleared first: a hash appended to a previous one would be a hash of two worlds. The
/// schema must be frozen — an unfrozen one has an order that depends on when declarations arrived,
/// and although this walk does not iterate the schema itself, refusing here is what stops a caller
/// from hashing before it has finished declaring.
///
/// `providers`, when given, is folded in as `Subsystem` nodes *inside* the world node and before
/// the archetypes — the position `simulation-and-determinism`'s hierarchy puts them in. They belong
/// in the same tree and not in a second one: "one moment, many consumers" means one hash, and two
/// trees would leave whoever compares them to decide how to combine the roots.
[[nodiscard]] Status hash_world(
    const ecs::World& world, const determinism::StateSchema& schema,
    determinism::StateHashTree& tree, WorldHashReport& report,
    const determinism::StateProviderRegistry* providers = nullptr) noexcept;

}  // namespace cy::runtime
