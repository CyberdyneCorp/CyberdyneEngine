#pragma once
// Resolution: turning an authoring graph into one concrete entity graph. Tasks 3.2.5, 3.2.6, 3.2.7.
//
// This is step one of cooking — "resolve nesting, variants, and overrides to a concrete entity
// graph" — and it is also what the editor runs to show a designer what a prefab instance actually
// contains. One implementation, because two would disagree, and the place they would disagree is
// precisely the place a designer would notice: what a value is and where it came from.
//
// --- THE ORDER OF APPLICATION, WHICH IS THE SEMANTICS
// ---------------------------------------------
//
// Expanding one placement of source S into container C:
//
//   1. resolve S (recursively: its own base chain and its own instances)
//   2. renumber S's entities into C's id space, through the placement's **mapping**
//   3. apply the placement's parameter **arguments**, writing every field each parameter is bound
//   to
//   4. apply the placement's explicit **overrides**
//
// Three before four, so an explicit override wins over a parameter that drives the same field. That
// is the answer that matches what a designer did: setting a field directly is a more specific act
// than setting a parameter that happens to reach it, and the more specific act should win. It is
// also the answer that keeps "internals stay private" true — a prefab may re-bind a parameter to a
// different field without changing what any instance's explicit overrides do.
//
// A variant is the same four steps with the document itself as the container: a variant is "a
// prefab whose base is another prefab, storing only its own overrides", which is a placement of one
// document into another with no separate instance entity. Sharing the code is why a variant of a
// variant of a prefab behaves the same as an instance of an instance, rather than nearly the same.
//
// --- THE MAPPING, AND WHAT IS NOT IN IT
// -----------------------------------------------------------
//
// A placement's mapping gives a source entity a stable local id in the container. Anything a
// reference points at **must** be in the mapping, because that is what makes the reference survive
// an edit to the source. Anything not in it is given a deterministic id at resolve time, derived
// from the container's own counter and the source order — stable for a given pair of documents, and
// not stable across an edit to the source, which is exactly why nothing may reference one.
//
// `populate_mapping()` fills a placement's mapping completely, and that is what an editor calls
// when it places a prefab. A hand-written document may leave it empty and rely on the
// auto-assignment; a document produced by a tool should not.
//
// --- PROVENANCE IS PART OF THE MODEL
// --------------------------------------------------------------
//
// "Provenance SHALL be part of the data model rather than an editor-only presentation, so it is
// available to validation, review tooling, and diff output." Every field of every resolved
// component therefore carries where its value came from, and the component keeps the value the
// layer below would have given it — so "what is the inherited value" is a lookup rather than a
// second resolve with the top layer removed.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/registry.h>
#include <cy/core/serialize/migration.h>
#include <cy/core/serialize/value_record.h>
#include <cy/scene/serialization/library.h>

#include <string_view>

namespace cy::scene::serialization {

/// How the cooker finds an entity's local transform.
///
/// A transform is a component like any other, and this module must not care which one a project
/// uses. So the caller names the component and the field within it that holds a `cy::Transform` —
/// forty bytes of rotation, translation and scale — and everything that composes or bakes a
/// transform goes through this binding.
///
/// It is a `FieldId` and a byte blob rather than nine reflected scalars because M1's reflection has
/// no vector field kind: a `Vec3` member is an opaque run to it. That is the seam, and when the
/// generator learns about vectors this becomes three field descriptors instead of one.
struct TransformBinding {
    reflect::TypeId component;
    reflect::FieldId field;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return component.valid() && field.valid();
    }
};

/// Where one field's value came from.
struct FieldProvenance {
    reflect::FieldId field;
    Provenance provenance;
};

/// One component on one resolved entity.
struct ResolvedComponent {
    reflect::TypeId type;
    /// The effective value: base, then every layer above it, in order.
    serialize::ValueRecord record;
    /// For each field a layer above the base overwrote, the value the layer below had. What
    /// "and what the inherited value is" means when it is data rather than a second resolve.
    serialize::ValueRecord inherited;
    Array<FieldProvenance> provenance;
};

/// One entity of the concrete graph.
struct ResolvedEntity {
    LocalId id;
    LocalId parent;
    TextRef name;
    MotionKind motion = MotionKind::Static;
    FlattenPolicy flatten = FlattenPolicy::Automatic;
    /// The document that contributed this entity, and its id there. Development-build provenance:
    /// it is what live prefab update diffs against, and it is stripped from a shipping cook.
    AssetId origin;
    LocalId origin_local;
    Array<ResolvedComponent> components;
};

/// One exposed parameter as it stands after resolution, with its bindings in this graph's id space.
struct ResolvedParameter {
    ParameterId id;
    TextRef name;
    serialize::WireType wire = serialize::WireType::Bytes;
    /// The effective value: the declared default, then every re-default a variant applied, then any
    /// argument a placement supplied.
    Array<u8> value;
    bool has_value = false;
    Array<ParameterBinding> bindings;
};

/// A concrete entity graph: no instances, no variants, no overrides.
class ResolvedGraph {
public:
    explicit ResolvedGraph(Allocator& allocator) noexcept
        : entities_(allocator), parameters_(allocator), text_(allocator) {}

    ResolvedGraph(const ResolvedGraph&) = delete;
    ResolvedGraph& operator=(const ResolvedGraph&) = delete;
    ResolvedGraph(ResolvedGraph&&) noexcept = default;
    ResolvedGraph& operator=(ResolvedGraph&&) noexcept = default;
    ~ResolvedGraph() = default;

    AssetId source;
    AssetKind kind = AssetKind::Scene;

    [[nodiscard]] Array<ResolvedEntity>& entities() noexcept { return entities_; }
    [[nodiscard]] const Array<ResolvedEntity>& entities() const noexcept { return entities_; }
    [[nodiscard]] Array<ResolvedParameter>& parameters() noexcept { return parameters_; }
    [[nodiscard]] const Array<ResolvedParameter>& parameters() const noexcept {
        return parameters_;
    }

    [[nodiscard]] ResolvedEntity* find(LocalId id) noexcept;
    [[nodiscard]] const ResolvedEntity* find(LocalId id) const noexcept;
    [[nodiscard]] ResolvedParameter* find_parameter(ParameterId id) noexcept;

    [[nodiscard]] Expected<TextRef, Error> intern(std::string_view value) noexcept;
    [[nodiscard]] std::string_view text(TextRef ref) const noexcept;

    /// Add an entity, keeping the list in ascending id order.
    [[nodiscard]] Expected<ResolvedEntity*, Error> add(LocalId id) noexcept;

    /// Where one value came from, or null when the entity, component or field is not in the graph.
    [[nodiscard]] const Provenance* provenance_of(LocalId entity, reflect::TypeId component,
                                                  reflect::FieldId field) const noexcept;
    /// The value the layer below would have given, or an empty span when nothing overrode it.
    [[nodiscard]] Span<const u8> inherited_value(LocalId entity, reflect::TypeId component,
                                                 reflect::FieldId field) const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return entities_.allocator(); }

private:
    Array<ResolvedEntity> entities_;
    Array<ResolvedParameter> parameters_;
    Array<char> text_;
};

/// The component with this type on this entity, or null. Shared with the cooker, because "is this
/// component present" must mean one thing.
[[nodiscard]] ResolvedComponent* find_resolved_component(ResolvedEntity& entity,
                                                         reflect::TypeId type) noexcept;
[[nodiscard]] const ResolvedComponent* find_resolved_component(const ResolvedEntity& entity,
                                                               reflect::TypeId type) noexcept;

/// The same, adding it when it is absent.
[[nodiscard]] Expected<ResolvedComponent*, Error> ensure_resolved_component(
    ResolvedEntity& entity, reflect::TypeId type, Allocator& allocator) noexcept;

/// Read an entity's local transform through the binding. The identity when the component or the
/// field is absent, which is what an entity with no authored placement means.
[[nodiscard]] Expected<cy::Transform, Error> read_transform_of(
    const ResolvedEntity& entity, const TransformBinding& binding) noexcept;

/// Write one, adding the component if it is not there and recording provenance.
[[nodiscard]] Status write_transform_of(ResolvedEntity& entity, const TransformBinding& binding,
                                        const cy::Transform& transform, AssetId asset,
                                        ValueSource source, Allocator& allocator) noexcept;

/// What a resolve was asked to do.
struct ResolveOptions {
    /// How to find a transform. Required to place a scene instance; a resolve without one reports
    /// `InvalidArgument` if it meets a placement with a non-identity transform, rather than
    /// silently putting the contents at the origin.
    TransformBinding transform;
    /// Used to migrate override targets whose fields have moved identity. Optional: without one, an
    /// override authored against an older schema is applied at the identifier it names.
    const serialize::SchemaRegistry* schemas = nullptr;
    /// Used to tell "this field was removed from the type" from "this field is simply not set on
    /// this instance". Optional, and the difference it makes is whether an override against a
    /// deleted field becomes a `MissingField` conflict or is applied to a record nobody reads.
    const reflect::TypeRegistry* types = nullptr;
};

/// One override that could not be applied, and where it lives so a person can act on it.
struct ConflictReport {
    /// The document holding the override — the container of the placement, or the variant itself.
    AssetId asset;
    /// The placement it belongs to, or `kNoLocalId` for a variant's own base overrides.
    LocalId instance;
    usize index = 0;
    ConflictKind kind = ConflictKind::None;
    OverrideTarget target;
};

/// What a resolve did. Numbers a cook report prints and a validation gate reads.
struct ResolveReport {
    explicit ResolveReport(Allocator& allocator) noexcept
        : chain(allocator), conflicts(allocator) {}

    u32 entities = 0;
    u32 overrides_applied = 0;
    u32 parameters_applied = 0;
    u32 instances_expanded = 0;
    u32 deepest_variant_chain = 0;
    /// True when some variant chain is deeper than the library's recommendation.
    bool variant_depth_warning = false;
    /// The chain that warned, so the warning names it.
    Array<AssetId> chain;
    Array<ConflictReport> conflicts;
};

/// Resolve one document into a concrete graph.
///
/// Overrides that cannot be applied are recorded in `report.conflicts` **and** marked on the
/// override in the authoring document, which is what "retained in the authoring data, surfaced in
/// the editor, reported by validation" needs. Nothing is ever discarded here.
[[nodiscard]] Status resolve(const Library& library, AssetId id, const ResolveOptions& options,
                             ResolvedGraph& out, ResolveReport& report) noexcept;

/// Fill a placement's mapping so that every entity the source contributes has a stable local id in
/// the container. What an editor calls when a prefab is dropped into a scene.
[[nodiscard]] Status populate_mapping(const Library& library, Document& container,
                                      Instance& instance, const ResolveOptions& options) noexcept;

/// The same, for a variant's base mapping.
[[nodiscard]] Status populate_base_mapping(const Library& library, Document& variant,
                                           const ResolveOptions& options) noexcept;

/// One difference between two resolved graphs.
struct GraphDifference {
    enum class Kind : u8 {
        EntityAdded,
        EntityRemoved,
        ComponentAdded,
        ComponentRemoved,
        FieldChanged,
        ParentChanged,
    };

    Kind kind = Kind::FieldChanged;
    LocalId entity;
    reflect::TypeId component;
    reflect::FieldId field;
};

/// One difference, with the members a given kind does not use left at their defaults. A named
/// constructor rather than a brace initialiser, because a partial brace initialiser is a warning
/// this tree builds as an error and spelling four members at every call site is noise.
[[nodiscard]] constexpr GraphDifference difference(GraphDifference::Kind kind, LocalId entity,
                                                   reflect::TypeId component = {},
                                                   reflect::FieldId field = {}) noexcept {
    return GraphDifference{kind, entity, component, field};
}

const char* difference_kind_name(GraphDifference::Kind kind) noexcept;

/// Diff two resolved graphs, "expressed as added, removed, and changed entities, components, and
/// fields" rather than as a text delta.
///
/// This is what a prefab review shows, what live prefab update consumes to build its runtime delta,
/// and what an instance-against-its-base comparison produces.
[[nodiscard]] Status diff(const ResolvedGraph& before, const ResolvedGraph& after,
                          Array<GraphDifference>& out) noexcept;

}  // namespace cy::scene::serialization
