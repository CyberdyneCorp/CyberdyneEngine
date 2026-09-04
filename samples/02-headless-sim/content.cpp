// The authoring pipeline, end to end. See content.h for the shape and the reasoning.

#include "content.h"

#include <cy/core/reflect/attributes.h>
#include <cy/scene/node.h>
#include <cy/scene/scene.h>
#include <cy/scene/serialization/cook.h>
#include <cy/scene/serialization/format.h>
#include <cy/scene/serialization/library.h>
#include <cy/scene/serialization/resolve.h>
#include <cy/scene/serialization/spawn.h>

#include <cstddef>
#include <cstdio>
#include <utility>

namespace sample {
namespace {

namespace ser = cy::scene::serialization;

using cy::Error;
using cy::Expected;
using cy::Span;
using cy::Status;
using cy::ecs::Entity;

// The identifiers. 9400 upwards: visibly not manifest-issued, and colliding with none of the three
// hand-written descriptor sets already in the tree.
constexpr u32 kPlacementType = 9401;
constexpr u32 kDriftType = 9402;
constexpr u32 kLinkType = 9403;

constexpr u32 kPlacementLocal = 1;
constexpr u32 kDriftX = 1;
constexpr u32 kDriftY = 2;
constexpr u32 kDriftLastTick = 3;
constexpr u32 kLinkEntity = 1;
constexpr u32 kLinkSlot = 2;

// The two authored documents. Numbers rather than paths because a document is addressed by asset
// id everywhere in this module; a sample has no asset database to resolve a path against.
constexpr cy::u64 kTurretAsset = 0x7011E7;
constexpr cy::u64 kSceneAsset = 0x5CE7E;

[[nodiscard]] cy::AssetId asset(cy::u64 low) noexcept {
    return {0, low};
}

[[nodiscard]] cy::reflect::FieldInfo make_field(const char* name, u32 id,
                                                cy::reflect::FieldKind kind, u32 offset, u32 size,
                                                cy::reflect::PersistenceKind persistence) noexcept {
    cy::reflect::FieldInfo field;
    field.name = name;
    field.id = cy::reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes.declared = cy::reflect::AttributeKind::Persistence;
    field.attributes.persistence = persistence;
    return field;
}

[[nodiscard]] ser::TransformBinding placement_binding() noexcept {
    return ser::TransformBinding{cy::reflect::TypeId(kPlacementType),
                                 cy::reflect::FieldId(kPlacementLocal)};
}

// --- Authoring
// ------------------------------------------------------------------------------------

/// Write a `cy::Transform` into an entity's `Placement`.
[[nodiscard]] Status set_placement(ser::DocumentEntity& entity,
                                   const cy::Transform& transform) noexcept {
    const Expected<ser::ComponentData*, Error> component =
        entity.ensure(cy::reflect::TypeId(kPlacementType));
    if (!component) {
        return cy::make_unexpected(component.error());
    }
    return (*component)
        ->record.set(cy::reflect::FieldId(kPlacementLocal), cy::serialize::WireType::Bytes,
                     &transform, static_cast<u32>(sizeof(transform)));
}

[[nodiscard]] Status set_drift(ser::DocumentEntity& entity, f32 x, f32 y) noexcept {
    const Expected<ser::ComponentData*, Error> component =
        entity.ensure(cy::reflect::TypeId(kDriftType));
    if (!component) {
        return cy::make_unexpected(component.error());
    }
    if (Status written = (*component)
                             ->record.set_scalar(cy::reflect::FieldId(kDriftX),
                                                 cy::serialize::WireType::F32, &x, sizeof(x));
        !written) {
        return written;
    }
    return (*component)
        ->record.set_scalar(cy::reflect::FieldId(kDriftY), cy::serialize::WireType::F32, &y,
                            sizeof(y));
}

/// The local ids `author_turret` issued. Held rather than assumed, because a parameter binding and
/// an override both address an entity by its id, and inserting one entity into the prefab would
/// silently repoint every hard-coded number.
struct TurretIds {
    ser::LocalId base;
    ser::LocalId skirt;
    ser::LocalId yaw;
    ser::LocalId muzzle;
};

/// The `Turret` prefab: a base, a yaw that rotates, and a muzzle bolted to the yaw.
///
/// The muzzle is `Static` under a `Dynamic` yaw on purpose. A per-edge flattening test would remove
/// the muzzle's relationship — it never moves relative to its parent — and the muzzle would then
/// stay put while the yaw turned. The rule is a walk to the root, and this prefab is the smallest
/// composition that tells the two apart.
[[nodiscard]] Status author_turret(ser::Document& document, TurretIds& ids) noexcept {
    document.kind = ser::AssetKind::Prefab;
    document.id = asset(kTurretAsset);

    const Expected<ser::DocumentEntity*, Error> base = document.add_entity(ser::kNoLocalId, "Base");
    if (!base) {
        return cy::make_unexpected(base.error());
    }
    if (Status placed = set_placement(**base, cy::Transform::identity()); !placed) {
        return placed;
    }
    if (Status drifted = set_drift(**base, 0.0F, 0.0F); !drifted) {
        return drifted;
    }
    ids.base = (*base)->id;

    // A skirt welded to the base. Nothing in the chain above it ever moves, so the cook removes its
    // relationship and bakes its transform — the flattening the storage argument is priced on, at
    // the smallest size that still shows it happening beside a relationship that survives.
    const Expected<ser::DocumentEntity*, Error> skirt = document.add_entity((*base)->id, "Skirt");
    if (!skirt) {
        return cy::make_unexpected(skirt.error());
    }
    if (Status placed =
            set_placement(**skirt, cy::Transform::from_translation(cy::Vec3{0.0F, -0.25F, 0.0F}));
        !placed) {
        return placed;
    }
    if (Status drifted = set_drift(**skirt, 0.0F, 0.0F); !drifted) {
        return drifted;
    }
    ids.skirt = (*skirt)->id;

    const Expected<ser::DocumentEntity*, Error> yaw = document.add_entity((*base)->id, "Yaw");
    if (!yaw) {
        return cy::make_unexpected(yaw.error());
    }
    (*yaw)->motion = ser::MotionKind::Dynamic;
    if (Status placed =
            set_placement(**yaw, cy::Transform::from_translation(cy::Vec3{0.0F, 1.0F, 0.0F}));
        !placed) {
        return placed;
    }
    if (Status drifted = set_drift(**yaw, 0.5F, 0.0F); !drifted) {
        return drifted;
    }
    ids.yaw = (*yaw)->id;

    const Expected<ser::DocumentEntity*, Error> muzzle = document.add_entity((*yaw)->id, "Muzzle");
    if (!muzzle) {
        return cy::make_unexpected(muzzle.error());
    }
    if (Status placed =
            set_placement(**muzzle, cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, 2.0F}));
        !placed) {
        return placed;
    }
    if (Status drifted = set_drift(**muzzle, 0.0F, 0.5F); !drifted) {
        return drifted;
    }
    ids.muzzle = (*muzzle)->id;

    // The muzzle points at the yaw it is bolted to. An intra-prefab reference, which is what makes
    // the cooked reference-site table observable: the spawn has to rewrite it per instance, and
    // every instance's muzzle must end up pointing at its OWN yaw.
    const Expected<ser::ComponentData*, Error> link =
        (*muzzle)->ensure(cy::reflect::TypeId(kLinkType));
    if (!link) {
        return cy::make_unexpected(link.error());
    }
    return (*link)->record.set_local_reference(cy::reflect::FieldId(kLinkEntity),
                                               (*yaw)->id.value());
}

/// A `Height` parameter on the turret, driving the yaw's and the muzzle's placements.
///
/// One parameter, two fields, two entities — the specification's "one parameter, many fields" at
/// the smallest size that still proves it, and the reason a scene instance can raise a whole turret
/// without knowing which entities the prefab happens to be made of.
[[nodiscard]] Status expose_height(ser::Document& turret, const TurretIds& ids,
                                   f32 initial) noexcept {
    const Expected<ser::ExposedParameter*, Error> parameter =
        turret.add_parameter("Height", cy::serialize::WireType::Bytes);
    if (!parameter) {
        return cy::make_unexpected(parameter.error());
    }
    const cy::Transform placement = cy::Transform::from_translation(cy::Vec3{0.0F, initial, 0.0F});
    if (Status kept = (*parameter)
                          ->default_value()
                          .append(Span<const cy::u8>(reinterpret_cast<const cy::u8*>(&placement),
                                                     sizeof(placement)));
        !kept) {
        return kept;
    }
    for (const ser::LocalId entity : {ids.yaw, ids.muzzle}) {
        if (Status bound =
                (*parameter)
                    ->bindings()
                    .push_back(ser::ParameterBinding{entity, cy::reflect::TypeId(kPlacementType),
                                                     cy::reflect::FieldId(kPlacementLocal)});
            !bound) {
            return bound;
        }
    }
    return cy::ok();
}

/// Supply an argument for an exposed parameter, by identifier and never by name.
[[nodiscard]] Status set_height(ser::Instance& instance, ser::ParameterId parameter, f32 height,
                                cy::Allocator& allocator) noexcept {
    ser::ParameterArgument argument;
    argument.id = parameter;
    argument.wire = cy::serialize::WireType::Bytes;
    argument.value = cy::Array<cy::u8>(allocator);
    const cy::Transform placement = cy::Transform::from_translation(cy::Vec3{0.0F, height, 0.0F});
    if (Status kept = argument.value.append(
            Span<const cy::u8>(reinterpret_cast<const cy::u8*>(&placement), sizeof(placement)));
        !kept) {
        return kept;
    }
    return instance.arguments().push_back(std::move(argument));
}

/// Override one scalar field of one entity contributed by an instance.
[[nodiscard]] Status override_drift_x(ser::Instance& instance, ser::LocalId target,
                                      f32 value) noexcept {
    ser::Override item(instance.overrides().allocator());
    item.set_op(ser::OverrideOp::SetField);
    item.set_target(ser::OverrideTarget{target, cy::reflect::TypeId(kDriftType),
                                        cy::reflect::FieldId(kDriftX)});
    item.payload().set_type(cy::reflect::TypeId(kDriftType));
    if (Status written = item.payload().set_scalar(
            cy::reflect::FieldId(kDriftX), cy::serialize::WireType::F32, &value, sizeof(value));
        !written) {
        return written;
    }
    return instance.overrides().add(std::move(item));
}

/// The `Emplacement` scene: two turrets, placed apart, one of them raised and one of them tuned.
[[nodiscard]] Status author_scene(ser::Document& scene, const ser::Document& turret,
                                  ser::ParameterId height) noexcept {
    scene.kind = ser::AssetKind::Scene;
    scene.id = asset(kSceneAsset);

    const Expected<ser::Instance*, Error> left =
        scene.add_instance(turret.id, ser::kNoLocalId, "Left");
    if (!left) {
        return cy::make_unexpected(left.error());
    }
    (*left)->transform = cy::Transform::from_translation(cy::Vec3{-4.0F, 0.0F, 0.0F});
    if (Status raised = set_height(**left, height, 3.5F, scene.allocator()); !raised) {
        return raised;
    }

    const Expected<ser::Instance*, Error> right =
        scene.add_instance(turret.id, ser::kNoLocalId, "Right");
    if (!right) {
        return cy::make_unexpected(right.error());
    }
    (*right)->transform = cy::Transform::from_translation(cy::Vec3{4.0F, 0.0F, 0.0F});
    return cy::ok();
}

/// Fill both placements' mappings and add the one override, which needs a mapping to address.
///
/// Separate from `author_scene` because it needs the library: a mapping is the container's answer
/// to "which of my local ids did this placement's entities get", and only the library knows what
/// the placed document contains.
[[nodiscard]] Status place_instances(const ser::Library& library, ser::Document& scene,
                                     const ser::ResolveOptions& options,
                                     const TurretIds& ids) noexcept {
    for (ser::Instance& instance : scene.instances()) {
        if (Status mapped = ser::populate_mapping(library, scene, instance, options); !mapped) {
            return mapped;
        }
    }
    // The right-hand turret's yaw drifts twice as fast. Authored against the PREFAB's own local id,
    // which is what an override addresses; the resolve maps it through the placement.
    return override_drift_x(scene.instances()[1], ids.yaw, 1.0F);
}

// --- The node scene
// -------------------------------------------------------------------------------

// --- The pipeline, one function per phase
// ------------------------------------------------------------

/// Phase 1. The two documents and the parameter that ties them together.
[[nodiscard]] Status author(ser::Document& turret, ser::Document& scene, TurretIds& ids) noexcept {
    if (Status authored = author_turret(turret, ids); !authored) {
        return authored;
    }
    if (Status exposed = expose_height(turret, ids, 1.0F); !exposed) {
        return exposed;
    }
    const ser::ExposedParameter* height = turret.find_parameter("Height");
    if (height == nullptr) {
        return cy::fail(cy::ErrorCode::Internal, "the parameter was declared and is not there");
    }
    return author_scene(scene, turret, height->id);
}

/// Phase 2. Write the authoring form, and read it back into `turret_in` and `scene_in`.
///
/// Everything after this works from what the READER produced. A round trip nothing depends on is a
/// round trip that can quietly stop working.
[[nodiscard]] Status round_trip(const ser::Document& turret, const ser::Document& scene,
                                ser::Document& turret_in, ser::Document& scene_in,
                                cy::Array<char>* text, ContentReport& report) noexcept {
    cy::Array<char> turret_text(turret.allocator());
    cy::Array<char> scene_text(scene.allocator());
    if (Status written = ser::write_text(turret, turret_text); !written) {
        return written;
    }
    if (Status written = ser::write_text(scene, scene_text); !written) {
        return written;
    }
    report.text_bytes = static_cast<u32>(turret_text.size() + scene_text.size());
    report.text_digest = digest(turret_text.data(), turret_text.size()) ^
                         digest(scene_text.data(), scene_text.size());
    if (text != nullptr) {
        if (Status kept = text->append(turret_text.span()); !kept) {
            return kept;
        }
        if (Status kept = text->append(scene_text.span()); !kept) {
            return kept;
        }
    }

    if (Status read =
            ser::read_text(std::string_view(turret_text.data(), turret_text.size()), turret_in);
        !read) {
        return read;
    }
    if (Status read =
            ser::read_text(std::string_view(scene_text.data(), scene_text.size()), scene_in);
        !read) {
        return read;
    }
    report.prefab_entities = static_cast<u32>(turret_in.entities().size());
    report.scene_placements = static_cast<u32>(scene_in.instances().size());
    report.parameters = static_cast<u32>(turret_in.parameters().size());
    return cy::ok();
}

/// Phase 3. Resolve the placements, flatten what never moves, and emit archetype blocks.
///
/// `layouts` is filled from the world rather than declared a second time: the runtime already knows
/// every component's size, alignment and entity offsets, and a second declaration is a second thing
/// to keep in step.
[[nodiscard]] Status cook_scene(const cy::ecs::World& world, ser::Document& turret_in,
                                ser::Document& scene_in, const TurretIds& ids,
                                ser::ComponentLayoutTable& layouts, ser::CookedAsset& cooked,
                                ContentReport& report) noexcept {
    ser::Library library(layouts.allocator());
    if (Status added = library.add(turret_in); !added) {
        return added;
    }
    if (Status added = library.add(scene_in); !added) {
        return added;
    }

    ser::CookOptions options;
    options.resolve.transform = placement_binding();
    // A conflict here would mean an override addressing a field the type no longer has, which for a
    // scene the sample authored in the same process is a defect rather than a migration.
    options.fail_on_conflicts = true;
    if (Status placed = place_instances(library, scene_in, options.resolve, ids); !placed) {
        return placed;
    }
    if (Status described = ser::describe_from_world(world, layouts); !described) {
        return described;
    }
    options.layouts = &layouts;

    ser::CookReport cook_report(layouts.allocator());
    if (Status baked = ser::cook(library, scene_in.id, options, cooked, cook_report); !baked) {
        return baked;
    }
    report.resolved_entities = cook_report.resolve.entities;
    report.overrides_applied = cook_report.resolve.overrides_applied;
    report.parameters_applied = cook_report.resolve.parameters_applied;
    report.conflicts = static_cast<u32>(cook_report.resolve.conflicts.size());
    report.blocks = cook_report.blocks;
    report.relationships_retained = cook_report.relationships_retained;
    report.relationships_flattened = cook_report.relationships_flattened;
    report.reference_sites = cook_report.reference_sites;
    report.dangling_references = cook_report.dangling_references;
    report.payload_bytes = cook_report.payload_bytes;
    return cy::ok();
}

/// Phase 4. Bind the cooked blocks to this world's component ids, once, and copy them in.
///
/// The batch spawn is one instantiation per archetype for the whole batch, which is the difference
/// between "spawns as a copy" and a thousand separate spawn calls.
[[nodiscard]] Status spawn(cy::ecs::World& world, const ser::ComponentLayoutTable& layouts,
                           const ser::CookedAsset& cooked, u32 instances, cy::Array<Entity>& out,
                           ContentReport& report) noexcept {
    ser::EntityTemplate entity_template(world.allocator());
    if (Status bound = entity_template.bind(world, layouts, cooked); !bound) {
        return bound;
    }
    if (Status spawned = entity_template.spawn_many(world, instances, out); !spawned) {
        return spawned;
    }
    report.instances = instances;
    report.entities = static_cast<u32>(out.size());
    return cy::ok();
}

/// A name of the form `<stem>-<index>`, in a caller-owned buffer.
[[nodiscard]] cy::Name numbered(char* buffer, usize capacity, const char* stem,
                                u32 index) noexcept {
    (void)std::snprintf(buffer, capacity, "%s-%u", stem, index);
    return cy::Name::intern(buffer);
}

}  // namespace

// --- The reflected descriptors
// --------------------------------------------------------------------

const cy::reflect::TypeInfo& placement_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        // `FieldKind::Unsupported` is what M1's reflection calls a run of bytes it cannot describe,
        // and it round-trips exactly through `WireType::Bytes`. It is also, deliberately, not
        // hashable — see simulation.h, which declares the ten floats inside it by hand.
        make_field("local", kPlacementLocal, cy::reflect::FieldKind::Unsupported,
                   static_cast<u32>(offsetof(Placement, local)), sizeof(cy::Transform),
                   cy::reflect::PersistenceKind::Authoring),
    };
    static const cy::reflect::TypeInfo info = [] {
        cy::reflect::TypeInfo type;
        type.name = "sample::Placement";
        type.id = cy::reflect::TypeId(kPlacementType);
        type.size = static_cast<u32>(sizeof(Placement));
        type.alignment = static_cast<u32>(alignof(Placement));
        type.trivially_relocatable = true;
        type.fields = fields;
        type.field_count = 1;
        return type;
    }();
    return info;
}

const cy::reflect::TypeInfo& drift_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        make_field("x", kDriftX, cy::reflect::FieldKind::F32, static_cast<u32>(offsetof(Drift, x)),
                   sizeof(f32), cy::reflect::PersistenceKind::RuntimeState),
        make_field("y", kDriftY, cy::reflect::FieldKind::F32, static_cast<u32>(offsetof(Drift, y)),
                   sizeof(f32), cy::reflect::PersistenceKind::RuntimeState),
        make_field("last_tick", kDriftLastTick, cy::reflect::FieldKind::U32,
                   static_cast<u32>(offsetof(Drift, last_tick)), sizeof(u32),
                   cy::reflect::PersistenceKind::Derived),
    };
    static const cy::reflect::TypeInfo info = [] {
        cy::reflect::TypeInfo type;
        type.name = "sample::Drift";
        type.id = cy::reflect::TypeId(kDriftType);
        type.size = static_cast<u32>(sizeof(Drift));
        type.alignment = static_cast<u32>(alignof(Drift));
        type.trivially_relocatable = true;
        type.fields = fields;
        type.field_count = 3;
        return type;
    }();
    return info;
}

const cy::reflect::TypeInfo& link_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        make_field("entity", kLinkEntity, cy::reflect::FieldKind::U64,
                   static_cast<u32>(offsetof(Link, entity)), sizeof(u64),
                   cy::reflect::PersistenceKind::Authoring),
        make_field("slot", kLinkSlot, cy::reflect::FieldKind::U32,
                   static_cast<u32>(offsetof(Link, slot)), sizeof(u32),
                   cy::reflect::PersistenceKind::Authoring),
    };
    static const cy::reflect::TypeInfo info = [] {
        cy::reflect::TypeInfo type;
        type.name = "sample::Link";
        type.id = cy::reflect::TypeId(kLinkType);
        type.size = static_cast<u32>(sizeof(Link));
        type.alignment = static_cast<u32>(alignof(Link));
        type.trivially_relocatable = true;
        type.fields = fields;
        type.field_count = 2;
        return type;
    }();
    return info;
}

Expected<Components, Error> register_components(cy::ecs::World& world) noexcept {
    Components ids;
    const Expected<cy::ecs::ComponentTypeId, Error> placement =
        world.components().register_reflected(placement_type());
    if (!placement) {
        return cy::make_unexpected(placement.error());
    }
    ids.placement = placement.value();

    const Expected<cy::ecs::ComponentTypeId, Error> drift =
        world.components().register_reflected(drift_type());
    if (!drift) {
        return cy::make_unexpected(drift.error());
    }
    ids.drift = drift.value();

    // The declared entity offset is the whole of "references are fixed up as a strided pass": the
    // component says where its `Entity` fields are, and nothing asks reflection again per row.
    const u32 offsets[] = {static_cast<u32>(offsetof(Link, entity))};
    cy::ecs::ComponentOptions options;
    options.entity_offsets = Span<const u32>(offsets, 1);
    const Expected<cy::ecs::ComponentTypeId, Error> link =
        world.components().register_reflected(link_type(), options);
    if (!link) {
        return cy::make_unexpected(link.error());
    }
    ids.link = link.value();
    return ids;
}

// --- The pipeline
// ---------------------------------------------------------------------------------

Status build_content(cy::ecs::World& world, u32 instances, cy::Array<char>* text,
                     cy::Array<Entity>& out, ContentReport& report) noexcept {
    cy::Allocator& allocator = world.allocator();

    ser::Document turret(allocator);
    ser::Document scene(allocator);
    TurretIds turret_ids;
    if (Status authored = author(turret, scene, turret_ids); !authored) {
        return authored;
    }

    // The reloaded pair is what everything below works from. `turret` and `scene` are dead from
    // here on, which is the whole point of the round trip being in the pipeline rather than in a
    // test beside it.
    ser::Document turret_in(allocator);
    ser::Document scene_in(allocator);
    if (Status reloaded = round_trip(turret, scene, turret_in, scene_in, text, report); !reloaded) {
        return reloaded;
    }

    ser::ComponentLayoutTable layouts(allocator);
    ser::CookedAsset cooked(allocator);
    if (Status baked = cook_scene(world, turret_in, scene_in, turret_ids, layouts, cooked, report);
        !baked) {
        return baked;
    }
    return spawn(world, layouts, cooked, instances, out, report);
}

Status build_node_scene(cy::scene::SceneTree& tree, u32 batteries, u32 turrets_per_battery,
                        NodeReport& report) noexcept {
    cy::Array<cy::scene::NodeDesc> nodes(tree.allocator());
    if (Status reserved = nodes.reserve(usize{1} + (batteries * (usize{1} + turrets_per_battery)));
        !reserved) {
        return reserved;
    }

    char buffer[64];
    cy::scene::NodeDesc field;
    field.name = cy::Name::intern("field");
    if (Status added = nodes.push_back(field); !added) {
        return added;
    }

    for (u32 battery = 0; battery < batteries; ++battery) {
        cy::scene::NodeDesc desc;
        desc.name = numbered(buffer, sizeof(buffer), "battery", battery);
        desc.parent = 0;
        desc.local_transform = cy::Transform::from_translation(
            cy::Vec3{static_cast<f32>(battery) * 10.0F, 0.0F, 0.0F});
        const u32 parent_index = static_cast<u32>(nodes.size());
        if (Status added = nodes.push_back(desc); !added) {
            return added;
        }
        for (u32 turret = 0; turret < turrets_per_battery; ++turret) {
            cy::scene::NodeDesc child;
            child.name = numbered(buffer, sizeof(buffer), "turret", turret);
            child.parent = parent_index;
            child.local_transform = cy::Transform::from_translation(
                cy::Vec3{0.0F, 0.0F, static_cast<f32>(turret) * 2.0F});
            if (Status added = nodes.push_back(child); !added) {
                return added;
            }
        }
    }

    cy::scene::SceneDescription description;
    description.name = cy::Name::intern(kNodeSceneName);
    description.nodes = nodes.span();
    const Expected<cy::scene::SceneId, Error> loaded = tree.load(description);
    if (!loaded) {
        return cy::make_unexpected(loaded.error());
    }

    report.scene = loaded.value();
    report.nodes = static_cast<u32>(nodes.size());
    report.batteries = batteries;
    report.turrets_per_battery = turrets_per_battery;

    // The battery nodes, resolved once by path through the façade. The sweep system moves exactly
    // these, so it does not walk a query every tick to find three entities the load already knew.
    for (u32 battery = 0; battery < batteries; ++battery) {
        (void)std::snprintf(buffer, sizeof(buffer), "%s/battery-%u", kNodeRootPath, battery);
        const cy::scene::Node node = tree.find(buffer);
        if (!node.valid()) {
            return cy::fail(cy::ErrorCode::NotFound,
                            "a battery node the load created is not there");
        }
        if (Status kept = report.batteries_entities.push_back(node.entity()); !kept) {
            return kept;
        }
    }
    return cy::ok();
}

u64 digest(const void* bytes, usize size) noexcept {
    // FNV-1a. Not a cryptographic hash and not the state hash — this exists so two processes can
    // agree that they authored the same text, and one line of output is cheaper to compare than a
    // whole file.
    constexpr u64 kOffsetBasis = 14695981039346656037ULL;
    constexpr u64 kPrime = 1099511628211ULL;
    const auto* data = static_cast<const cy::u8*>(bytes);
    u64 hash = kOffsetBasis;
    for (usize index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= kPrime;
    }
    return hash;
}

}  // namespace sample
