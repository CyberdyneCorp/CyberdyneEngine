#pragma once
// Persistence traits: what a save captures, derived from the one declaration that already exists.
// Task 6.1.
//
// `save-and-persistence` — "Persistence traits" — requires that fields declare traits "extending
// the field classification already defined in `serialization-and-prefabs`", that the traits are
// "the ONLY mechanism" deciding what a save captures, and that "a save SHALL NOT be assembled by a
// hand-maintained list of what to write".
//
// THE TRAITS ARE DERIVED, NOT DECLARED A SECOND TIME. `reflect::PersistenceKind`, `Transient` and
// `Replicated` are already on every field, and `cy/core/serialize/classification.h` is already the
// one table turning a class into a decision. A second declaration would be a second source of truth
// about whether a field is saved, and the two would drift the first time somebody added a field
// and updated one of them. So `traits_of()` READS the existing declaration and reports it as the
// vocabulary the specification uses, and `field_is_saved()` is `serialize::field_is_written(field,
// Purpose::Persistence)` with no table of its own — src/save/tests/test_traits.cpp asserts the two
// agree for every combination, so a change to either is a failing test rather than a silent
// divergence.
//
//   trait            derived from
//   ---------------  ------------------------------------------------------------------
//   Authoring        PersistenceKind::Authoring
//   SaveGame         PersistenceKind::PersistentState
//   RuntimeOnly      PersistenceKind::RuntimeState
//   Derived          PersistenceKind::Derived
//   Replicated       the Replicated attribute, which is a different declaration and says so
//   ReplayRelevant   written for Purpose::Snapshot — everything a verbatim capture contains
//   Profile          the SaveScope attribute below, when a project declares one
//
// WHY `Profile` IS AN ATTRIBUTE AND THE OTHERS ARE NOT. The other six are answerable from what the
// reflection module already carries. Which SCOPE a saved field belongs to is not: a graphics
// setting and a quest flag are both `PersistentState`, and nothing in the type system distinguishes
// them. `core-type-system` provides for exactly this — "a module that needs an attribute the table
// does not have declares its own in an attribute schema ... reached through find_custom<T>() rather
// than by parsing a string" — so scope routing is a custom attribute, and its absence means
// `Scope::World`, which is what the overwhelming majority of saved fields are.

#include <cy/core/base/types.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>
#include <cy/core/serialize/classification.h>
#include <cy/save/identity.h>

namespace cy::save {

/// The specification's vocabulary, as a bit set. A field may carry several.
enum class Trait : u16 {
    None = 0,
    /// Defined by the asset. Updated when the asset changes; not carried by a save.
    Authoring = 1U << 0U,
    /// Written to the persistence overlay. This is the trait a save is assembled from.
    SaveGame = 1U << 1U,
    /// Belongs to a scope with a lifetime longer than the campaign.
    Profile = 1U << 2U,
    /// Sent to peers. A different declaration, and `networking-and-replication` owns its meaning.
    Replicated = 1U << 3U,
    /// Part of a verbatim capture of live state: a snapshot, a replay frame, a play-mode reset.
    ReplayRelevant = 1U << 4U,
    /// Owned by the running simulation and preserved across an asset update, but never saved.
    RuntimeOnly = 1U << 5U,
    /// Computed. Never serialised, reconstructed on load, and absent from every column.
    Derived = 1U << 6U,
};

constexpr Trait operator|(Trait a, Trait b) noexcept {
    return static_cast<Trait>(static_cast<u16>(a) | static_cast<u16>(b));
}
constexpr Trait& operator|=(Trait& a, Trait b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool has_trait(Trait set, Trait one) noexcept {
    return (static_cast<u16>(set) & static_cast<u16>(one)) != 0U;
}

/// The generated form of the `SaveScope` attribute: one scope, as its enumerator's number.
///
/// A project declares it in an attribute schema (`tools/gen/attributes/`) and the generator emits
/// exactly this struct beside the field; `find_custom<T>()` checks the size before handing one
/// back, so a schema that changed under this build reads as absent rather than as other bytes.
struct SaveScopeAttribute {
    u8 scope = static_cast<u8>(Scope::World);
};

/// The name the attribute is emitted under. One spelling, in one place.
inline constexpr const char* kSaveScopeAttribute = "SaveScope";

/// Every trait this field carries, from the declaration it already has.
[[nodiscard]] Trait traits_of(const reflect::FieldInfo& field) noexcept;

/// Which scope this field's value belongs to. `Scope::World` unless the field says otherwise.
[[nodiscard]] Scope scope_of(const reflect::FieldInfo& field) noexcept;

/// True when a save captures this field.
///
/// One line, and deliberately: the classification table in `cy/core/serialize/classification.h` is
/// the only copy of this rule, and a second predicate here that reasoned about persistence kinds
/// itself would be the hand-maintained list the specification forbids.
[[nodiscard]] constexpr bool field_is_saved(const reflect::FieldInfo& field) noexcept {
    return serialize::field_is_written(field, serialize::Purpose::Persistence);
}

/// True when a save of `scope` captures this field. The scope routing on top of the trait test.
[[nodiscard]] inline bool field_is_saved_in(const reflect::FieldInfo& field, Scope scope) noexcept {
    return field_is_saved(field) && scope_of(field) == scope;
}

}  // namespace cy::save
