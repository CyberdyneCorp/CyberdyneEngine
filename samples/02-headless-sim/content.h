// The content: three reflected components, an authored prefab and scene, and the cook that lowers
// them into archetype blocks. samples/02-headless-sim, task 5.1.
//
// THE PIPELINE THIS HEADER DECLARES, IN THE ORDER IT RUNS:
//
//   author    a `Turret` prefab (base / yaw / muzzle, one exposed parameter, an intra-prefab
//             reference) and an `Emplacement` scene that places it twice with different arguments
//             and one override. Documents, not entities: this is the authoring form.
//   text      the scene is written to the text form and READ BACK, and everything after this line
//             works from what the reader produced. A round trip that is only tested is a round trip
//             that the artefact does not depend on; this one it does.
//   resolve   variants, overrides and parameters collapse into one concrete graph.
//   flatten   every relationship whose subtree never moves is removed and its transform baked.
//   cook      the graph becomes archetype blocks laid out for M1's chunks, plus the reference-site
//             table that makes activation a strided pass rather than a reflection walk.
//   spawn     the blocks are bound to the world's component ids once and copied in.
//
// WHY THE COMPONENT DESCRIPTORS ARE HAND-WRITTEN. A `reflect::TypeInfo` is plain constexpr data and
// the generator emits exactly this shape; what it cannot yet emit is a descriptor for a type
// declared outside `cy_reflect_annotated_headers`, whose identifiers would have to come from
// identity/manifest.toml. src/ecs/tests/fixtures.h, src/scene/serialization/tests/fixtures.h and
// src/runtime/probe/tick_loop_probe.cpp all record the same seam. Identifiers start at 9400 so a
// number here is visibly not one the manifest issued and collides with none of those.

#ifndef CY_SAMPLE_HEADLESS_SIM_CONTENT_H
#define CY_SAMPLE_HEADLESS_SIM_CONTENT_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/world.h>
#include <cy/scene/tree.h>

namespace sample {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::usize;

// --- The components ------------------------------------------------------------------------------

/// Where a cooked entity sits, as one forty-byte field.
///
/// One field rather than ten because M1's reflection has no vector kind — a `Vec3` member is an
/// opaque run of bytes to it — and `TransformBinding` is the seam the cooker composes and bakes
/// through. The state schema declares the ten floats inside it explicitly (simulation.h), which is
/// the other half of the same gap.
struct Placement {
    cy::Transform local;
};

/// What the simulation integrates. Two authoritative floats and one derived witness.
struct Drift {
    f32 x = 0.0F;
    f32 y = 0.0F;
    /// Derived: rewritten every tick from the clock, never hashed, never saved.
    u32 last_tick = 0;
};

/// An entity reference at a declared byte offset, and a plain field beside it so the reference is
/// not the whole component. The declared offset is what makes the cooked reference-site table
/// possible: a field is a reference precisely when the component said its offset was one.
struct Link {
    u64 entity = 0;
    u32 slot = 0;
};

const cy::reflect::TypeInfo& placement_type() noexcept;
const cy::reflect::TypeInfo& drift_type() noexcept;
const cy::reflect::TypeInfo& link_type() noexcept;

/// The ids one world gave the three components. Resolved once; nothing below looks a type up again.
struct Components {
    cy::ecs::ComponentTypeId placement = cy::ecs::kInvalidComponent;
    cy::ecs::ComponentTypeId drift = cy::ecs::kInvalidComponent;
    cy::ecs::ComponentTypeId link = cy::ecs::kInvalidComponent;
};

[[nodiscard]] cy::Expected<Components, cy::Error> register_components(
    cy::ecs::World& world) noexcept;

// --- The authoring pipeline ----------------------------------------------------------------------

/// What the pipeline did, in the order it did it. Every figure is printed, and every figure is a
/// function of the content rather than of the machine — which is what lets the smoke test compare
/// two processes line for line.
struct ContentReport {
    // Authored, and round-tripped through the text form.
    u32 prefab_entities = 0;
    u32 scene_placements = 0;
    u32 parameters = 0;
    u32 text_bytes = 0;
    /// FNV-1a over the text the writer produced. Two runs that authored the same scene agree.
    u64 text_digest = 0;

    // Resolved: variants, overrides and parameters collapsed into one graph.
    u32 resolved_entities = 0;
    u32 overrides_applied = 0;
    u32 parameters_applied = 0;
    u32 conflicts = 0;

    // Cooked: archetype blocks, and what flattening decided.
    u32 blocks = 0;
    u32 relationships_retained = 0;
    u32 relationships_flattened = 0;
    u32 reference_sites = 0;
    u32 dangling_references = 0;
    u32 payload_bytes = 0;

    // Spawned.
    u32 instances = 0;
    u32 entities = 0;
};

/// Author the scene, round-trip it through its text form, cook it, and spawn `instances` copies.
///
/// `out` receives every spawned entity, instance-major: instance `i`'s entity for template index
/// `t` is `out[i * entities_per_instance + t]`.
///
/// `text`, when not null, receives the authoring text form the writer produced — what
/// `--show-scene` prints. The round trip itself is in memory: a sample that needed a file on disk
/// to reproduce a hash would be measuring the disk.
[[nodiscard]] cy::Status build_content(cy::ecs::World& world, u32 instances, cy::Array<char>* text,
                                       cy::Array<cy::ecs::Entity>& out,
                                       ContentReport& report) noexcept;

// --- The node hierarchy --------------------------------------------------------------------------

/// What the node scene is, once loaded.
struct NodeReport {
    u32 scene = 0;
    u32 nodes = 0;
    u32 batteries = 0;
    u32 turrets_per_battery = 0;
    /// The entities of the battery nodes, which is the set the sweep system moves.
    /// Held here because a system that walks a whole query to find three entities every tick is a
    /// system doing a lookup the load already did.
    cy::Array<cy::ecs::Entity> batteries_entities;

    explicit NodeReport(cy::Allocator& allocator) noexcept : batteries_entities(allocator) {}
};

/// The node scene's name, and where its contents sit in the tree.
///
/// A load adds exactly one node under the tree root, named for the scene, so that unloading takes
/// the whole scene with it and an additive load cannot leave orphans behind. An authored node's
/// path therefore carries the scene's name in front of it, which is worth writing down once rather
/// than discovering at the first `find()` that returns nothing.
inline constexpr const char* kNodeSceneName = "emplacement";
inline constexpr const char* kNodeRootPath = "/emplacement/field";

/// Load `batteries` battery nodes, each with `turrets_per_battery` turrets, under one root.
///
/// The authored half of the milestone that the cook does not cover: a named tree with a transform
/// per node, loaded through the façade rather than assembled component by component.
[[nodiscard]] cy::Status build_node_scene(cy::scene::SceneTree& tree, u32 batteries,
                                          u32 turrets_per_battery, NodeReport& report) noexcept;

/// FNV-1a over a byte range. Used for the text digest, and for nothing that has to be secure.
[[nodiscard]] u64 digest(const void* bytes, usize size) noexcept;

}  // namespace sample

#endif  // CY_SAMPLE_HEADLESS_SIM_CONTENT_H
