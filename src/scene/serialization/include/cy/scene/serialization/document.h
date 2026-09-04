#pragma once
// The authoring document: one file's worth of entities, instances and parameters. Tasks 3.2.1,
// 3.2.3, 3.2.4, 3.2.5 and 3.2.7.
//
// This is the artefact a designer's tool edits and version control stores. It is *not* what the
// runtime loads: cooking (cook.h) resolves it into archetype blocks in which none of this structure
// survives. Everything here therefore optimises for being read, diffed and merged by people, and
// nothing here optimises for being fast.
//
// --- WHAT ONE FILE HOLDS
// --------------------------------------------------------------------------
//
// "Authoring data SHALL be stored one file per authoring unit: a prefab asset, a scene asset, or a
// chunk of a world region containing hundreds to a few thousand entities. One file per entity SHALL
// NOT be used." A `Document` is one such unit. Because identity is a local id rather than a
// position or a path, an entity moves between documents without changing identity — which is what
// lets an oversized authoring chunk be split without breaking a single reference.
//
// --- THE FOUR THINGS A DOCUMENT CONTAINS
// ----------------------------------------------------------
//
//   entities     authored directly in this document. Each has a local id, a name, a parent, a
//                motion classification, a flattening policy, and its components as value records.
//   instances    a placement of another document — a prefab instance, or (in a world) a scene
//                instance with a cook mode. Carries its overrides, its parameter arguments, and the
//                **mapping** from the source's local ids to this document's.
//   parameters   a prefab's deliberate public interface: named, typed, documented, and each bound
//   to
//                one or more fields across one or more entities.
//   a base       for a prefab variant: the prefab it specialises, with its own mapping, overrides
//                and parameter re-defaults. A variant is a prefab whose base is another prefab and
//                which stores only its own overrides — the same machinery as an instance, applied
//                to the document as a whole.
//
// --- WHY AN INSTANCE CARRIES A MAPPING
// ------------------------------------------------------------
//
// When a prefab is placed, every entity it will contribute is given a local id **in this
// document**, once, and the mapping records it. Two things follow, and both are requirements:
//
//   * a reference from anywhere to an entity inside an instance is an ordinary local id, so
//     "reordering does not break references" holds for instanced content as well as authored
//     content;
//   * re-placing, re-saving or re-cooking never renumbers anything, so a scene's diff after an
//     unrelated edit is empty.
//
// The overrides do *not* use the mapping: an override addresses the **prefab-local** id, because it
// is authored against the prefab and must survive the prefab being re-laid-out internally. The
// mapping is how resolution turns the one into the other, and it is the only place the two id
// spaces meet.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/serialize/value_record.h>
#include <cy/core/serialize/wire.h>
#include <cy/scene/serialization/asset.h>
#include <cy/scene/serialization/overrides.h>

#include <string_view>

namespace cy::scene::serialization {

/// A slice of a document's text pool: a name, as stored.
///
/// Names live in one pool per document rather than in a per-entity allocation or in the global
/// intern table. A pool costs one allocation for a whole file and cannot leak a designer's strings
/// into a process-wide table that never shrinks; interning would buy fast comparison, which nothing
/// on this path needs, because a name is metadata and every lookup that matters is by identifier.
struct TextRef {
    u32 offset = 0;
    u32 length = 0;
};

/// One component on one authored entity, as a value record.
struct ComponentData {
    reflect::TypeId type;
    serialize::ValueRecord record;
};

/// One entity as authored.
class DocumentEntity {
public:
    explicit DocumentEntity(Allocator& allocator) noexcept : components_(allocator) {}

    DocumentEntity(const DocumentEntity&) = delete;
    DocumentEntity& operator=(const DocumentEntity&) = delete;
    DocumentEntity(DocumentEntity&&) noexcept = default;
    DocumentEntity& operator=(DocumentEntity&&) noexcept = default;
    ~DocumentEntity() = default;

    LocalId id;
    LocalId parent;
    TextRef name;
    MotionKind motion = MotionKind::Static;
    FlattenPolicy flatten = FlattenPolicy::Automatic;

    [[nodiscard]] Array<ComponentData>& components() noexcept { return components_; }
    [[nodiscard]] const Array<ComponentData>& components() const noexcept { return components_; }

    [[nodiscard]] ComponentData* find(reflect::TypeId type) noexcept;
    [[nodiscard]] const ComponentData* find(reflect::TypeId type) const noexcept;

    /// Add a component, or return the one already there. Components are held in `TypeId` order, so
    /// two documents describing the same entity write the same file.
    [[nodiscard]] Expected<ComponentData*, Error> ensure(reflect::TypeId type) noexcept;
    bool remove(reflect::TypeId type) noexcept;

private:
    Array<ComponentData> components_;
};

/// One entry of an instance's id mapping: a local id in the source document, and the local id this
/// document gave the entity it contributes.
struct InstanceMapping {
    LocalId source;
    LocalId local;
};

/// A value supplied for an exposed parameter. Carries the identifier, never the name.
struct ParameterArgument {
    ParameterId id;
    serialize::WireType wire = serialize::WireType::Bytes;
    /// The encoded value, in the same little-endian form a value record holds.
    Array<u8> value;
};

/// One field an exposed parameter drives.
struct ParameterBinding {
    /// The entity's id in the prefab that declares the parameter.
    LocalId entity;
    reflect::TypeId component;
    reflect::FieldId field;
};

/// A prefab's deliberate public interface: "a named, typed, documented set of values".
class ExposedParameter {
public:
    explicit ExposedParameter(Allocator& allocator) noexcept
        : default_value_(allocator), bindings_(allocator) {}

    ExposedParameter(const ExposedParameter&) = delete;
    ExposedParameter& operator=(const ExposedParameter&) = delete;
    ExposedParameter(ExposedParameter&&) noexcept = default;
    ExposedParameter& operator=(ExposedParameter&&) noexcept = default;
    ~ExposedParameter() = default;

    ParameterId id;
    TextRef name;
    TextRef documentation;
    serialize::WireType wire = serialize::WireType::Bytes;

    [[nodiscard]] Array<u8>& default_value() noexcept { return default_value_; }
    [[nodiscard]] const Array<u8>& default_value() const noexcept { return default_value_; }
    [[nodiscard]] Array<ParameterBinding>& bindings() noexcept { return bindings_; }
    [[nodiscard]] const Array<ParameterBinding>& bindings() const noexcept { return bindings_; }

private:
    Array<u8> default_value_;
    Array<ParameterBinding> bindings_;
};

/// A placement of another document inside this one.
///
/// One type for both a prefab instance and a scene instance. They differ in the kind of the
/// document they name and in whether `cook_mode` means anything — a prefab instance is always
/// embedded into whatever contains it, and a scene instance chooses. Making them one type is what
/// keeps the resolver from having two nearly identical expansions that can disagree.
class Instance {
public:
    explicit Instance(Allocator& allocator) noexcept
        : mapping_(allocator), overrides_(allocator), arguments_(allocator) {}

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) noexcept = default;
    Instance& operator=(Instance&&) noexcept = default;
    ~Instance() = default;

    /// This instance's own identity in the containing document.
    LocalId id;
    /// Where the instance's roots attach. `kNoLocalId` puts them at the document's root.
    LocalId parent;
    TextRef name;
    /// The document being placed.
    AssetId source;
    /// Scenes are authored in local coordinates; this is what places them.
    cy::Transform transform;
    CookMode cook_mode = CookMode::Embedded;

    [[nodiscard]] Array<InstanceMapping>& mapping() noexcept { return mapping_; }
    [[nodiscard]] const Array<InstanceMapping>& mapping() const noexcept { return mapping_; }
    [[nodiscard]] OverrideList& overrides() noexcept { return overrides_; }
    [[nodiscard]] const OverrideList& overrides() const noexcept { return overrides_; }
    [[nodiscard]] Array<ParameterArgument>& arguments() noexcept { return arguments_; }
    [[nodiscard]] const Array<ParameterArgument>& arguments() const noexcept { return arguments_; }

    /// The local id this instance gave the source's entity, or `kNoLocalId`.
    [[nodiscard]] LocalId local_of(LocalId source_id) const noexcept;

private:
    Array<InstanceMapping> mapping_;
    OverrideList overrides_;
    Array<ParameterArgument> arguments_;
};

/// One authoring file.
class Document {
public:
    explicit Document(Allocator& allocator) noexcept
        : entities_(allocator),
          instances_(allocator),
          parameters_(allocator),
          base_mapping_(allocator),
          base_overrides_(allocator),
          base_arguments_(allocator),
          text_(allocator) {}

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;
    ~Document() = default;

    AssetKind kind = AssetKind::Scene;
    AssetId id;
    /// The document format's own schema version, distinct from any type's.
    u16 schema_version = 0;

    // --- The variant base ---------------------------------------------------------------------

    /// The prefab this one specialises, or the nil id. Prefabs only.
    [[nodiscard]] AssetId base() const noexcept { return base_; }
    void set_base(AssetId base) noexcept { base_ = base; }
    [[nodiscard]] bool is_variant() const noexcept { return !base_.is_nil(); }

    [[nodiscard]] Array<InstanceMapping>& base_mapping() noexcept { return base_mapping_; }
    [[nodiscard]] const Array<InstanceMapping>& base_mapping() const noexcept {
        return base_mapping_;
    }
    [[nodiscard]] OverrideList& base_overrides() noexcept { return base_overrides_; }
    [[nodiscard]] const OverrideList& base_overrides() const noexcept { return base_overrides_; }
    /// A variant's parameter re-defaults: the same shape as an instance's arguments, applied one
    /// level lower. This is the "re-defaulted by the variant, then set by the instance" case.
    [[nodiscard]] Array<ParameterArgument>& base_arguments() noexcept { return base_arguments_; }
    [[nodiscard]] const Array<ParameterArgument>& base_arguments() const noexcept {
        return base_arguments_;
    }

    // --- Identity -----------------------------------------------------------------------------

    /// Issue the next local id. Monotonic and never reused, which is the whole contract.
    [[nodiscard]] LocalId allocate_id() noexcept { return LocalId(next_local_id_++); }
    [[nodiscard]] u32 next_local_id() const noexcept { return next_local_id_; }
    /// Restore the counter when reading a file, so ids issued afterwards do not collide with ids
    /// already in it.
    void set_next_local_id(u32 next) noexcept { next_local_id_ = next; }

    // --- Text pool ----------------------------------------------------------------------------

    [[nodiscard]] Expected<TextRef, Error> intern(std::string_view value) noexcept;
    [[nodiscard]] std::string_view text(TextRef ref) const noexcept;

    // --- Entities -----------------------------------------------------------------------------

    /// Add an entity with a fresh id. Entities are held in ascending id order.
    [[nodiscard]] Expected<DocumentEntity*, Error> add_entity(LocalId parent,
                                                              std::string_view name) noexcept;
    /// Add an entity at an id the caller chose. What reading a file does.
    [[nodiscard]] Expected<DocumentEntity*, Error> add_entity_with_id(
        LocalId entity_id, LocalId parent, std::string_view name) noexcept;

    [[nodiscard]] DocumentEntity* find_entity(LocalId entity_id) noexcept;
    [[nodiscard]] const DocumentEntity* find_entity(LocalId entity_id) const noexcept;
    [[nodiscard]] Array<DocumentEntity>& entities() noexcept { return entities_; }
    [[nodiscard]] const Array<DocumentEntity>& entities() const noexcept { return entities_; }

    // --- Instances ----------------------------------------------------------------------------

    [[nodiscard]] Expected<Instance*, Error> add_instance(AssetId source, LocalId parent,
                                                          std::string_view name) noexcept;
    [[nodiscard]] Expected<Instance*, Error> add_instance_with_id(LocalId instance_id,
                                                                  AssetId source, LocalId parent,
                                                                  std::string_view name) noexcept;
    [[nodiscard]] Instance* find_instance(LocalId instance_id) noexcept;
    [[nodiscard]] const Instance* find_instance(LocalId instance_id) const noexcept;
    [[nodiscard]] Array<Instance>& instances() noexcept { return instances_; }
    [[nodiscard]] const Array<Instance>& instances() const noexcept { return instances_; }

    // --- Exposed parameters -------------------------------------------------------------------

    [[nodiscard]] Expected<ExposedParameter*, Error> add_parameter(
        std::string_view name, serialize::WireType wire) noexcept;
    [[nodiscard]] Expected<ExposedParameter*, Error> add_parameter_with_id(
        ParameterId parameter_id, std::string_view name, serialize::WireType wire) noexcept;
    [[nodiscard]] ExposedParameter* find_parameter(ParameterId parameter_id) noexcept;
    [[nodiscard]] const ExposedParameter* find_parameter(ParameterId parameter_id) const noexcept;
    /// By name — for the editor and for authoring tools only. Nothing that persists a reference
    /// uses this, which is why arguments carry the identifier.
    [[nodiscard]] const ExposedParameter* find_parameter(std::string_view name) const noexcept;
    [[nodiscard]] Array<ExposedParameter>& parameters() noexcept { return parameters_; }
    [[nodiscard]] const Array<ExposedParameter>& parameters() const noexcept { return parameters_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return entities_.allocator(); }

private:
    Array<DocumentEntity> entities_;
    Array<Instance> instances_;
    Array<ExposedParameter> parameters_;

    AssetId base_;
    Array<InstanceMapping> base_mapping_;
    OverrideList base_overrides_;
    Array<ParameterArgument> base_arguments_;

    Array<char> text_;
    /// One is the first id issued: zero is "no entity", so it is never an entity's.
    u32 next_local_id_ = 1;
    u32 next_parameter_id_ = 1;
};

/// The local id an instance gave a source entity, or `kNoLocalId`. Free function so it reads the
/// same for a base mapping, which is not held on an `Instance`.
[[nodiscard]] LocalId mapped_local(Span<const InstanceMapping> mapping, LocalId source) noexcept;

/// Add a mapping entry, keeping the list in source order.
[[nodiscard]] Status add_mapping(Array<InstanceMapping>& mapping, LocalId source,
                                 LocalId local) noexcept;

}  // namespace cy::scene::serialization
