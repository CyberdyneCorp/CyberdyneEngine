#include <cy/runtime/state_hash.h>

#include <cy/core/determinism/ordering.h>
#include <cy/core/determinism/random.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/archetype.h>
#include <cy/ecs/component.h>

namespace cy::runtime {
namespace {

using determinism::HashLevel;
using determinism::StateField;
using determinism::StateHashTree;
using determinism::SubjectSchema;

/// A component type's identity, as a number that is the same in every build that has the type.
///
/// **NOT the `ComponentTypeId`.** That is the index `ComponentRegistry` handed out, which is
/// registration order — a per-world sequence. Folding it into the hash made two worlds of identical
/// content hash differently when their components had been registered in a different order, which
/// was measured at M2's close and is exactly what `simulation-and-determinism` forbids:
/// "Registries whose contents affect simulation — systems, types, rules, providers — SHALL be
/// finalised in a deterministic order derived from **stable identifiers**", and "WHEN plugins load
/// in a different order THEN simulation results SHALL be unchanged". Build-time feature slicing is
/// the case that would have found it: dropping one module's components shifts every later id, so a
/// sliced build could not compare hashes with a full one.
///
/// A reflected component has a `reflect::TypeId` from the identity manifest, which is the stable
/// identifier the requirement means. A built-in is registered by name with no descriptor, so its
/// name is hashed — with `determinism::detail::hash_name`, the unseeded compile-time FNV-1a the
/// random streams key on, and deliberately **not** `cy::hash_bytes`, which is seeded per process in
/// development builds and would make the hash differ between two runs of the same binary.
[[nodiscard]] u64 stable_component_key(const ecs::ComponentRegistry& registry,
                                       ecs::ComponentTypeId component) noexcept {
    const ecs::ComponentInfo& info = registry.info(component);
    if (info.type_id.value() != 0) {
        return static_cast<u64>(info.type_id.value());
    }
    return determinism::detail::hash_name(info.name);
}

/// Sort a scratch list of tokens ascending, so that folding it is order-independent.
void sort_tokens(Array<u64>& tokens) noexcept {
    determinism::sort_by_key(
        tokens.size(), [&](usize i) noexcept { return tokens[i]; },
        [&](usize i) noexcept { return tokens[i]; },
        [&](usize a, usize b) noexcept {
            const u64 held = tokens[a];
            tokens[a] = tokens[b];
            tokens[b] = held;
        });
}

/// The archetypes' walk order, as a value that depends only on which components an archetype holds.
///
/// Folded from the components' stable identities, so two builds that registered the same types in
/// different orders produce the same key and walk the archetypes in the same sequence — which the
/// archetype's own id, being creation order, would not.
///
/// Shared component values are folded in as their interned index. That index is assigned in
/// interning order, which for one deterministic run of one program is itself deterministic; it is
/// the one place in this walk where the ordering rests on a sequence rather than on an identity,
/// and it is recorded here rather than left to be discovered.
[[nodiscard]] Expected<u64, Error> archetype_key(const ecs::ComponentRegistry& registry,
                                                 const ecs::Archetype& archetype,
                                                 Array<u64>& scratch) noexcept {
    // THE TOKENS ARE SORTED BEFORE THEY ARE FOLDED, AND THAT IS THE SECOND HALF OF THE FIX.
    // `archetype.components()` walks the column order, which `ecs-core` fixes as ascending
    // *component id* — registration order again. Using stable identities but folding them in column
    // order still made two worlds of identical content disagree, because `fold_hash` is
    // order-dependent by construction. Sorting the identities makes the key a function of the
    // archetype's component *set*, which is what it claims to be.
    scratch.clear();
    for (const ecs::ComponentTypeId component : archetype.components()) {
        if (Status pushed = scratch.push_back(stable_component_key(registry, component)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (const ecs::SharedValue& shared : archetype.shared()) {
        // The identity and the value fold together first, so the pair survives the sort as one
        // token rather than as two numbers that could be paired differently.
        const u64 token =
            determinism::fold_hash(stable_component_key(registry, shared.component), shared.value);
        if (Status pushed = scratch.push_back(token); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    sort_tokens(scratch);

    u64 key = 0x9e3779b97f4a7c15ULL;
    for (const u64 token : scratch) {
        key = determinism::fold_hash(key, token);
    }
    return key;
}

/// One archetype's components, in stable-identity order.
///
/// The column order would be the obvious walk and is the wrong one for the same reason the
/// archetype key is: `ecs-core` fixes columns as *sorted component id*, and a component id is
/// registration order. Sorted once per archetype and reused for every entity in it.
[[nodiscard]] Status collect_component_order(const ecs::ComponentRegistry& registry,
                                             const ecs::Archetype& archetype,
                                             Array<ecs::ComponentTypeId>& out,
                                             Array<u64>& out_keys) noexcept {
    out.clear();
    out_keys.clear();
    for (const ecs::ComponentTypeId component : archetype.components()) {
        if (Status pushed = out.push_back(component); !pushed) {
            return pushed;
        }
        if (Status keyed = out_keys.push_back(stable_component_key(registry, component)); !keyed) {
            return keyed;
        }
    }
    determinism::sort_by_key(
        out.size(), [&](usize i) noexcept { return out_keys[i]; },
        [&](usize i) noexcept { return static_cast<u64>(out[i]); },
        [&](usize a, usize b) noexcept {
            const ecs::ComponentTypeId held = out[a];
            const u64 held_key = out_keys[a];
            out[a] = out[b];
            out_keys[a] = out_keys[b];
            out[b] = held;
            out_keys[b] = held_key;
        });
    return ok();
}

/// Every archetype holding at least one entity, in the order the walk visits them.
///
/// Sorted by the content-derived key with the table index as the tie-break — `ordering.h`'s rule,
/// applied to the engine's own walk rather than only offered to gameplay code.
[[nodiscard]] Status collect_archetype_order(const ecs::ComponentRegistry& registry,
                                             const ecs::ArchetypeTable& archetypes,
                                             Array<u32>& order, Array<u64>& keys,
                                             Array<u64>& scratch) noexcept {
    for (u32 index = 0; index < archetypes.size(); ++index) {
        const ecs::Archetype& archetype = archetypes.at(index);
        if (archetype.entity_count() == 0) {
            continue;
        }
        if (Status pushed = order.push_back(index); !pushed) {
            return pushed;
        }
        const Expected<u64, Error> key = archetype_key(registry, archetype, scratch);
        if (!key) {
            return make_unexpected(key.error());
        }
        if (Status keyed = keys.push_back(*key); !keyed) {
            return keyed;
        }
    }
    determinism::sort_by_key(
        order.size(), [&](usize i) noexcept { return keys[i]; },
        [&](usize i) noexcept { return static_cast<u64>(order[i]); },
        [&](usize a, usize b) noexcept {
            const u32 held_order = order[a];
            const u64 held_key = keys[a];
            order[a] = order[b];
            keys[a] = keys[b];
            order[b] = held_order;
            keys[b] = held_key;
        });
    return ok();
}

/// Every live entity of one archetype, sorted by entity index.
///
/// The key columns are read chunk by chunk because that is where they are; the *order* is the
/// entity index, which is what makes the result independent of which chunk row an entity landed in.
[[nodiscard]] Status collect_entities(const ecs::Archetype& archetype,
                                      Array<ecs::Entity>& out) noexcept {
    out.clear();
    for (u32 chunk = 0; chunk < archetype.chunk_count(); ++chunk) {
        // `chunk()` is non-const on Archetype because a ChunkView is a mutable window. Nothing here
        // writes through it; the const_cast is confined to this line and to this reason.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        ChunkView view = const_cast<ecs::Archetype&>(archetype).chunk(chunk);
        for (const ecs::Entity entity : view.keys_as<ecs::Entity>()) {
            if (Status pushed = out.push_back(entity); !pushed) {
                return pushed;
            }
        }
    }
    determinism::sort_by_key(
        out.size(), [&](usize i) noexcept { return out[i].index(); },
        [&](usize i) noexcept { return static_cast<u64>(out[i].index()); },
        [&](usize a, usize b) noexcept {
            const ecs::Entity held = out[a];
            out[a] = out[b];
            out[b] = held;
        });
    return ok();
}

/// One component of one entity: a `Component` node holding a `Field` node per participating field.
[[nodiscard]] Status hash_component(StateHashTree& tree, const determinism::StateSchema& schema,
                                    const SubjectSchema& subject, const char* name, u64 stable_key,
                                    const void* value, WorldHashReport& report) noexcept {
    // The node's id is the component's stable identity, never its `ComponentTypeId`: see
    // `stable_component_key()`.
    if (Status opened = tree.begin(HashLevel::Component, stable_key, name); !opened) {
        return opened;
    }
    ++report.components_hashed;

    for (const StateField& field : schema.fields_of(subject)) {
        if (!determinism::participation_of(field.classification).hashed) {
            continue;
        }
        if (Status opened = tree.begin(HashLevel::Field, field.id, field.name); !opened) {
            return opened;
        }
        determinism::hash_field(tree, field, value);
        ++report.fields_hashed;
        if (Status closed = tree.end(); !closed) {
            return closed;
        }
    }
    return tree.end();
}

/// One entity: an `Entity` node holding a `Component` node per declared component it carries.
///
/// The node's id is the entity *index* and the generation is not mixed. The generation is recycling
/// history: two runs that destroyed and recreated the same entity in the same order agree about it,
/// and a run that legitimately reached the same state by another route does not. Hashing it would
/// report a divergence for a world in which every value a game can observe is identical.
[[nodiscard]] Status hash_entity(StateHashTree& tree, const ecs::World& world,
                                 const determinism::StateSchema& schema,
                                 Span<const ecs::ComponentTypeId> components,
                                 Span<const u64> component_keys, ecs::Entity entity,
                                 WorldHashReport& report) noexcept {
    if (Status opened = tree.begin(HashLevel::Entity, entity.index(), "entity"); !opened) {
        return opened;
    }
    ++report.entities_hashed;

    const ecs::ComponentRegistry& registry = world.components();
    for (usize index = 0; index < components.size(); ++index) {
        const ecs::ComponentTypeId component = components[index];
        const SubjectSchema* subject = schema.find(subject_of(component));
        if (subject == nullptr || subject->hashed_field_count == 0) {
            continue;
        }
        const void* value = world.get(entity, component);
        if (value == nullptr) {
            // A tag, a shared component or a sparse component: no column, so no bytes to read at
            // this row. A schema declared for one would be a schema whose fields address nothing,
            // which `declare()` cannot detect and this skip makes harmless.
            continue;
        }
        if (Status hashed = hash_component(tree, schema, *subject, registry.info(component).name,
                                           component_keys[index], value, report);
            !hashed) {
            return hashed;
        }
    }
    return tree.end();
}

/// The three arrays one archetype's walk needs, so that hashing a world allocates them once rather
/// than once per archetype.
struct ArchetypeScratch {
    explicit ArchetypeScratch(Allocator& allocator) noexcept
        : entities(allocator), components(allocator), component_keys(allocator) {}

    Array<ecs::Entity> entities;
    Array<ecs::ComponentTypeId> components;
    Array<u64> component_keys;
};

/// One archetype: an `Archetype` node holding an `Entity` node per live entity, entities in index
/// order and components in stable-identity order.
[[nodiscard]] Status hash_archetype(StateHashTree& tree, const ecs::World& world,
                                    const determinism::StateSchema& schema,
                                    const ecs::Archetype& archetype, u64 key,
                                    ArchetypeScratch& scratch, WorldHashReport& report) noexcept {
    ++report.archetypes_visited;
    if (Status collected = collect_entities(archetype, scratch.entities); !collected) {
        return collected;
    }
    if (Status ordered = collect_component_order(world.components(), archetype, scratch.components,
                                                 scratch.component_keys);
        !ordered) {
        return ordered;
    }
    if (Status opened = tree.begin(HashLevel::Archetype, key, "archetype"); !opened) {
        return opened;
    }
    const Span<const ecs::ComponentTypeId> components(scratch.components.data(),
                                                      scratch.components.size());
    const Span<const u64> component_keys(scratch.component_keys.data(),
                                         scratch.component_keys.size());
    for (const ecs::Entity entity : scratch.entities) {
        if (Status hashed =
                hash_entity(tree, world, schema, components, component_keys, entity, report);
            !hashed) {
            return hashed;
        }
    }
    return tree.end();
}

}  // namespace

Status declare_reflected_components(const ecs::World& world, determinism::StateSchema& schema,
                                    SchemaDeclarationReport& report) noexcept {
    report = SchemaDeclarationReport{};
    const ecs::ComponentRegistry& registry = world.components();
    for (u32 component = 0; component < registry.size(); ++component) {
        const ecs::ComponentInfo& info = registry.info(component);
        const determinism::SchemaSubject subject = subject_of(component);
        if (schema.find(subject) != nullptr) {
            ++report.already_declared;
            continue;
        }
        if (info.type == nullptr) {
            // A built-in: registered by name, with no reflected descriptor to derive fields from.
            // Counted rather than guessed at; see the header comment.
            ++report.skipped_unreflected;
            continue;
        }
        if (Status declared = schema.declare_reflected(subject, *info.type); !declared) {
            return declared;
        }
        ++report.declared;
    }
    return ok();
}

Status hash_world(const ecs::World& world, const determinism::StateSchema& schema,
                  StateHashTree& tree, WorldHashReport& report,
                  const determinism::StateProviderRegistry* providers) noexcept {
    report = WorldHashReport{};
    if (!schema.frozen()) {
        return fail(ErrorCode::Unavailable,
                    "the state schema is not frozen; hashing before every subject is declared "
                    "would produce a number whose meaning depends on how far registration got");
    }
    tree.clear();

    Allocator& allocator = world.allocator();
    const ecs::ArchetypeTable& archetypes = world.archetypes();

    Array<u32> order(allocator);
    Array<u64> keys(allocator);
    Array<u64> component_keys(allocator);
    if (Status collected =
            collect_archetype_order(world.components(), archetypes, order, keys, component_keys);
        !collected) {
        return collected;
    }

    if (Status opened = tree.begin(HashLevel::World, 0, world.name()); !opened) {
        return opened;
    }

    // Subsystems before archetypes, which is the hierarchy's own order.
    if (providers != nullptr) {
        if (Status subsystems = providers->hash_all(tree); !subsystems) {
            return subsystems;
        }
    }

    ArchetypeScratch scratch(allocator);
    for (usize position = 0; position < order.size(); ++position) {
        if (Status hashed = hash_archetype(tree, world, schema, archetypes.at(order[position]),
                                           keys[position], scratch, report);
            !hashed) {
            return hashed;
        }
    }

    if (Status closed = tree.end(); !closed) {
        return closed;
    }

    const ecs::ComponentRegistry& registry = world.components();
    for (u32 component = 0; component < registry.size(); ++component) {
        if (schema.find(subject_of(component)) != nullptr) {
            ++report.subjects_declared;
        } else {
            ++report.subjects_undeclared;
        }
    }
    report.hash = tree.root_hash();
    return ok();
}

}  // namespace cy::runtime
