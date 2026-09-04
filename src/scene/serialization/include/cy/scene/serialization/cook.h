#pragma once
// Cooking: the authoring graph in, archetype blocks out. Tasks 3.2.10 and 3.2.11.
//
// `serialization-and-prefabs` — "Scene and prefab cooking" — lists six steps and one prohibition,
// and `cook()` below is those six steps in that order:
//
//   1. resolve nesting, variants and overrides to a concrete entity graph   (resolve.h)
//   2. apply exposed parameter bindings                                     (resolve.h)
//   3. validate references and reject cycles
//   4. flatten hierarchy that is not needed at runtime
//   5. assign persistent identities
//   6. emit entity templates
//
// The prohibition is that "editor-only data — names, folder organisation, gizmo settings, selection
// state, comments — SHALL NOT be cooked into runtime data unless a runtime consumer requires it".
// Nothing below writes a name. A `CookedAsset` is column bytes, a relationship list, and — in a
// development build only — a provenance table for live update.
//
// --- HIERARCHY FLATTENING, AND THE TEST THAT IS NOT PER-EDGE
// --------------------------------------
//
// "Cooking SHALL determine, per parent-child relationship, whether the relationship is needed at
// runtime — because something animates, detaches, moves, or queries it — or is purely an authoring
// convenience."
//
// The M2 spike found the trap here, and it is worth stating because the obvious implementation gets
// it wrong: **the test is a walk to the root, not a look at one edge.** A static muzzle bolted to a
// static barrel still needs its relationship if the *yaw* three levels up rotates, because
// flattening the muzzle's edge bakes a world transform that stops being true the moment the yaw
// turns. So an edge is needed when the child moves **or when anything above it moves**. A per-edge
// test flattens the muzzle out from under the yaw, and the failure is visible only in motion.
//
// The decision is overridable per entity (`FlattenPolicy`), which is what a designer uses for the
// two cases the analysis cannot see: something welded on that the motion classification calls
// dynamic, and something the analysis calls static that a system will nonetheless reparent.
//
// --- WHY A COOKED BLOCK CARRIES A REFERENCE-SITE TABLE
// --------------------------------------------
//
// The spike measured this and the conclusion is in `<cy/core/serialize/cooked.h>`: a cooked block
// copies into a chunk as whole-column memcpys, but the key column and every entity-reference slot
// hold cook-time indices and must be rewritten afterwards. Emitting the sites — the column and the
// byte offset of each entity-typed field — makes that fixup a strided pass over known columns
// rather than a reflection walk, which the spike measured at 4.7 to 5.2 times the cost.
//
// A cooked reference slot holds **`template index + 1`**, and zero means "no entity". The bias is
// what makes a null reference and a reference to the first entity distinguishable in a slot that is
// zero-initialised, and it costs one increment at cook and one decrement at spawn.
//
// --- HOW A COMPONENT'S FIELDS FIND THEIR BYTES
// ----------------------------------------------------
//
// A `ComponentLayout` is a reflected type plus the byte offsets of its `Entity` fields, which is
// exactly what `ecs::ComponentInfo` holds — so `describe_from_world()` builds the table from a
// world that has already registered its components, and nothing has to be declared twice. **A field
// is an entity reference precisely when its byte offset is one of the declared offsets**; that is
// the one place the authoring model's `LocalRef` and the ECS's `entity_offsets` meet, and stating
// it once is what keeps them from meaning different things.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/serialize/cooked.h>
#include <cy/ecs/world.h>
#include <cy/scene/serialization/resolve.h>

namespace cy::scene::serialization {

/// How one component type is laid out in a chunk row, and where its entity references sit.
struct ComponentLayout {
    const reflect::TypeInfo* type = nullptr;
    u32 size = 0;
    u32 alignment = 1;
    /// Byte offsets, within one element, of the `Entity` fields it holds. Declared, never guessed.
    Array<u32> entity_offsets;
    /// Built once so that turning a value record into row bytes is a probe per field.
    reflect::FieldIndex fields;
};

/// The component types a cook may emit. Anything not in it is an unknown component: "ignored in
/// cooked formats", because cooked data has no tags and nothing to carry it in.
class ComponentLayoutTable {
public:
    explicit ComponentLayoutTable(Allocator& allocator) noexcept : layouts_(allocator) {}

    [[nodiscard]] Status add(const reflect::TypeInfo& type,
                             Span<const u32> entity_offsets) noexcept;
    [[nodiscard]] const ComponentLayout* find(reflect::TypeId type) const noexcept;
    [[nodiscard]] usize size() const noexcept { return layouts_.size(); }
    [[nodiscard]] Span<const ComponentLayout> layouts() const noexcept { return layouts_.span(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return layouts_.allocator(); }

private:
    Array<ComponentLayout> layouts_;
};

/// Build the table from a world's registered components, reflected descriptor and declared entity
/// offsets together. The route a real cook takes: the runtime already knows this, and a second
/// declaration is a second thing to keep in step.
[[nodiscard]] Status describe_from_world(const ecs::World& world,
                                         ComponentLayoutTable& out) noexcept;

/// One parent-child relationship that survived flattening, as template indices.
struct TemplateRelationship {
    u32 child = 0;
    u32 parent = 0;
};

/// Where one cooked entity came from. Development builds only: "prefab provenance SHALL be retained
/// in development builds for live update, and stripped from shipping builds".
struct TemplateOrigin {
    AssetId asset;
    LocalId local;
};

/// What a cook produced.
class CookedAsset {
public:
    explicit CookedAsset(Allocator& allocator) noexcept
        : stream_(allocator),
          row_indices_(allocator),
          relationships_(allocator),
          origins_(allocator) {}

    CookedAsset(const CookedAsset&) = delete;
    CookedAsset& operator=(const CookedAsset&) = delete;
    CookedAsset(CookedAsset&&) noexcept = default;
    CookedAsset& operator=(CookedAsset&&) noexcept = default;
    ~CookedAsset() = default;

    AssetId source;
    AssetKind kind = AssetKind::Prefab;
    /// The schema every packed byte in `stream` was produced against. A load whose runtime
    /// disagrees is a hard error, checked before a byte is copied.
    u64 build_schema = 0;
    u32 entity_count = 0;

    [[nodiscard]] Array<u8>& stream() noexcept { return stream_; }
    [[nodiscard]] const Array<u8>& stream() const noexcept { return stream_; }
    /// The template index of each cooked row, in the order the blocks and their rows are written.
    ///
    /// Emission groups entities by archetype, so a block's rows are not consecutive template
    /// indices — and the entities a spawn creates come back in block-and-row order. This is the map
    /// between the two, and without it a reference fixup would be writing entities into the wrong
    /// slots in exactly the cases where more than one archetype is involved.
    [[nodiscard]] Array<u32>& row_indices() noexcept { return row_indices_; }
    [[nodiscard]] const Array<u32>& row_indices() const noexcept { return row_indices_; }

    [[nodiscard]] Array<TemplateRelationship>& relationships() noexcept { return relationships_; }
    [[nodiscard]] const Array<TemplateRelationship>& relationships() const noexcept {
        return relationships_;
    }
    /// Empty in a shipping cook. `spawn.h` uses it for live prefab update and for nothing else.
    [[nodiscard]] Array<TemplateOrigin>& origins() noexcept { return origins_; }
    [[nodiscard]] const Array<TemplateOrigin>& origins() const noexcept { return origins_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return stream_.allocator(); }

private:
    Array<u8> stream_;
    Array<u32> row_indices_;
    Array<TemplateRelationship> relationships_;
    Array<TemplateOrigin> origins_;
};

/// What a cook was asked to do.
struct CookOptions {
    ResolveOptions resolve;
    /// Required: without it every component is unknown and the cook emits nothing.
    const ComponentLayoutTable* layouts = nullptr;
    /// Keep the provenance table. True for a development cook, false for shipping, where "the
    /// resulting entities SHALL carry no prefab link and no override data".
    bool retain_provenance = true;
    /// "Validation SHALL report them and, by configuration, fail the build."
    bool fail_on_conflicts = false;
};

/// The numbers a cook report prints. "The cook report SHALL state how many relationships were
/// flattened" is one of them, and the rest are the ones that turn out to matter when a cook is
/// unexpectedly large or slow.
struct CookReport {
    explicit CookReport(Allocator& allocator) noexcept : resolve(allocator) {}

    ResolveReport resolve;
    u32 entities = 0;
    u32 blocks = 0;
    u32 relationships_flattened = 0;
    u32 relationships_retained = 0;
    u32 reference_sites = 0;
    /// References that pointed outside the cooked graph and were written as null.
    u32 dangling_references = 0;
    /// Components with no layout: not an error, and not carried, because cooked data has no tags.
    u32 unknown_components = 0;
    u32 payload_bytes = 0;
};

/// The build schema identity of a layout table. Computed identically by the cooker and by whatever
/// loads its output, which is what makes the mismatch check mean anything.
[[nodiscard]] u64 build_schema_of(const ComponentLayoutTable& layouts) noexcept;

/// Cook one document into an entity template.
[[nodiscard]] Status cook(const Library& library, AssetId id, const CookOptions& options,
                          CookedAsset& out, CookReport& report) noexcept;

/// Cook a graph that has already been resolved. What the world cooker will call at M6 once it has
/// partitioned a resolved world into cells, and what a test uses to cook a graph it built by hand.
[[nodiscard]] Status cook_resolved(ResolvedGraph& graph, const CookOptions& options,
                                   CookedAsset& out, CookReport& report) noexcept;

/// Decide, for every entity, whether its relationship to its parent survives; bake the transform of
/// every one that does not.
///
/// Exposed because it is the step with the interesting rule and it deserves to be testable on its
/// own, without a cook around it.
[[nodiscard]] Status flatten_hierarchy(ResolvedGraph& graph, const TransformBinding& transform,
                                       CookReport& report) noexcept;

/// Null every reference that points outside the graph, counting them.
///
/// A reference to an entity that is not here is not an error — the specification's own answer for a
/// cross-scene reference is "it SHALL resolve to a null handle and re-resolve if that scene is
/// later loaded" — but it is worth counting, because a cook that nulls thousands of them is a cook
/// whose scene boundaries are in the wrong place.
[[nodiscard]] Status validate_references(ResolvedGraph& graph, CookReport& report) noexcept;

}  // namespace cy::scene::serialization
