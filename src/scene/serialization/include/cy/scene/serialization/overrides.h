#pragma once
// Instance overrides: what one is, what it addresses, and what happens when its target is gone.
// Tasks 3.2.5, 3.2.6 and 3.2.7.
//
// `serialization-and-prefabs` states three requirements this header implements together, because
// they are one design and separating them would let two of them drift.
//
//   "Instance overrides"  — an instance stores only its differences: per-property value overrides,
//                           added and removed components, added and removed child entities. Each is
//                           individually revertible. An override whose target no longer exists
//                           becomes an explicit **conflict**, "not a discarded value", and "SHALL
//                           NOT be dropped silently in any build configuration".
//   "Overrides address stable identifiers" — the prefab-local entity identifier, the component type
//                           identifier and the field identifier. Never a name path such as
//                           `Root.LeftArm.Weapon.Damage`. "Override operations SHALL be explicit
//                           and enumerable: set field, add component, remove component, add entity,
//                           remove entity, and reparent entity."
//   "Prefab diff and override provenance" — for any value the model reports where it came from and
//                           what the inherited value was, "as part of the data model rather than an
//                           editor-only presentation".
//
// --- WHY AN OVERRIDE IS ADDRESSED BY THREE NUMBERS AND NOTHING ELSE
// -------------------------------
//
// This is the first data in the engine whose correctness depends on an identifier having been
// assigned once and never reused, which is what M1's manifest and its tombstones exist for. A name
// path breaks on every rename and on every reorganisation of a prefab's internals — and the failure
// is silent, because a path that no longer resolves looks exactly like an override that was never
// authored. Three identifiers break on neither, and when a field's *identity* genuinely changes,
// migration moves the override with the data (`serialize::SchemaRegistry::migrate_field_id`).
//
// The `entity` half is the **prefab-local** id, not the containing document's. An override is
// authored against the prefab, and the same override applies to every instance of it.
//
// --- WHY A CONFLICT IS A VALUE RATHER THAN A LOG LINE
// ---------------------------------------------
//
// "Silently discarding an override discards work a designer did deliberately, in the one place
// where nobody is looking." So an override whose target has gone keeps its payload, records why it
// could not be applied, and offers the three resolutions the specification names: discard it,
// retarget it, or restore the removed structure. Nothing in this module ever erases an override on
// its own.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/serialize/value_record.h>
#include <cy/scene/serialization/asset.h>

namespace cy::scene::serialization {

/// The enumerable set of override operations. Exactly the six the specification lists.
enum class OverrideOp : u8 {
    SetField = 0,
    AddComponent = 1,
    RemoveComponent = 2,
    AddEntity = 3,
    RemoveEntity = 4,
    ReparentEntity = 5,
};

const char* override_op_name(OverrideOp op) noexcept;

/// Why an override could not be applied. `None` means it applied.
enum class ConflictKind : u8 {
    None = 0,
    /// The entity the override addresses is no longer in the prefab.
    MissingEntity = 1,
    /// The component is no longer on that entity.
    MissingComponent = 2,
    /// The field is no longer on that component's type.
    MissingField = 3,
    /// The entity an `AddEntity` or `ReparentEntity` names as the new parent is gone.
    MissingParent = 4,
};

const char* conflict_kind_name(ConflictKind kind) noexcept;

/// Where a value came from. Part of the data model, so validation, review tooling and diff output
/// all read the same answer the inspector shows.
enum class ValueSource : u8 {
    /// The base prefab at the bottom of the chain.
    Base = 0,
    /// An intermediate variant.
    Variant = 1,
    /// The instance's own override.
    Instance = 2,
    /// An exposed parameter binding wrote it.
    Parameter = 3,
    /// The cooker computed it — a baked world transform is the only one at M2. Not an authoring
    /// layer: it is here so that a value the cook wrote is never mistaken for a value a designer
    /// typed, which is exactly the confusion an inspector showing provenance exists to prevent.
    Cooked = 4,
};

const char* value_source_name(ValueSource source) noexcept;

/// Where one value came from, and what it would have been without the layer that set it.
struct Provenance {
    ValueSource source = ValueSource::Base;
    /// The asset that supplied the value: the base prefab, the variant, or the containing document.
    AssetId asset;
    /// True when a lower layer also supplied a value, so `inherited` is meaningful.
    bool overridden = false;
};

/// What an override addresses: three identifiers, and nothing that a rename can invalidate.
struct OverrideTarget {
    /// The entity's id **in the prefab being overridden**, not in the containing document.
    LocalId entity;
    reflect::TypeId component;
    /// Unset for the whole-component operations.
    reflect::FieldId field;

    friend constexpr bool operator==(const OverrideTarget&,
                                     const OverrideTarget&) noexcept = default;
};

/// One difference an instance stores from its source.
///
/// Move-only, because it owns a value record. `clone_into` is the explicit copy, which is what the
/// editor's "duplicate this instance" does.
class Override {
public:
    explicit Override(Allocator& allocator) noexcept : payload_(allocator) {}

    Override(const Override&) = delete;
    Override& operator=(const Override&) = delete;
    Override(Override&&) noexcept = default;
    Override& operator=(Override&&) noexcept = default;
    ~Override() = default;

    [[nodiscard]] OverrideOp op() const noexcept { return op_; }
    void set_op(OverrideOp op) noexcept { op_ = op; }

    [[nodiscard]] const OverrideTarget& target() const noexcept { return target_; }
    void set_target(const OverrideTarget& target) noexcept { target_ = target; }

    /// The value being set, or the component being added. Empty for a removal.
    [[nodiscard]] serialize::ValueRecord& payload() noexcept { return payload_; }
    [[nodiscard]] const serialize::ValueRecord& payload() const noexcept { return payload_; }

    /// `ReparentEntity`'s new parent, and `AddEntity`'s parent. `kNoLocalId` detaches to the root.
    [[nodiscard]] LocalId parent() const noexcept { return parent_; }
    void set_parent(LocalId parent) noexcept { parent_ = parent; }

    /// The schema version the override was authored against, so migration can walk its target up.
    [[nodiscard]] u16 schema_version() const noexcept { return schema_version_; }
    void set_schema_version(u16 version) noexcept { schema_version_ = version; }

    [[nodiscard]] ConflictKind conflict() const noexcept { return conflict_; }
    [[nodiscard]] bool conflicted() const noexcept { return conflict_ != ConflictKind::None; }
    void set_conflict(ConflictKind kind) noexcept { conflict_ = kind; }

    // --- The three resolutions the specification names
    // --------------------------------------------
    //
    // Each one is an act a person takes, so each is a call rather than a policy the resolver
    // applies on its own. Nothing here is called by resolution; resolution only ever *marks* a
    // conflict.

    /// Retarget the override at another field or entity, and clear the conflict.
    void resolve_retarget(const OverrideTarget& target) noexcept {
        target_ = target;
        conflict_ = ConflictKind::None;
    }
    /// Declare the conflict resolved because the structure it needs has been restored.
    void resolve_restored() noexcept { conflict_ = ConflictKind::None; }

    [[nodiscard]] Status clone_into(Override& out) const noexcept;

private:
    OverrideOp op_ = OverrideOp::SetField;
    OverrideTarget target_;
    serialize::ValueRecord payload_;
    LocalId parent_;
    u16 schema_version_ = 0;
    ConflictKind conflict_ = ConflictKind::None;
};

/// A list of overrides that never loses one.
///
/// `Array<Override>` would do everything this does; the reason it is a type is `remove`, which is
/// the only way an override leaves a list and is spelled `discard` so that a reviewer reading a
/// diff sees the word. Dropping an override is a designer's decision, and it should be legible as
/// one.
class OverrideList {
public:
    explicit OverrideList(Allocator& allocator) noexcept : items_(allocator) {}

    [[nodiscard]] Status add(Override&& item) noexcept { return items_.push_back(std::move(item)); }

    /// Discard one override. The only removal, and named for what it costs.
    bool discard(usize index) noexcept;

    [[nodiscard]] usize size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] Override& operator[](usize index) noexcept { return items_[index]; }
    [[nodiscard]] const Override& operator[](usize index) const noexcept { return items_[index]; }
    [[nodiscard]] Override* begin() noexcept { return items_.begin(); }
    [[nodiscard]] Override* end() noexcept { return items_.end(); }
    [[nodiscard]] const Override* begin() const noexcept { return items_.begin(); }
    [[nodiscard]] const Override* end() const noexcept { return items_.end(); }

    /// How many carry an unresolved conflict. What cook-time validation reports, and what a project
    /// configured to fail the build on conflicts fails on.
    [[nodiscard]] usize conflict_count() const noexcept;

    void clear() noexcept { items_.clear(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return items_.allocator(); }

private:
    Array<Override> items_;
};

}  // namespace cy::scene::serialization
