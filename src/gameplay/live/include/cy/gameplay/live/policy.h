#pragma once
// The live edit policy: what a change to one field means for a world that is already running.
// M11.b task 3.2.
//
// ================================================================================================
// THE REQUIREMENT, AND WHY IT HAD NO CODE UNTIL NOW
// ================================================================================================
//
// `live-editing` (Live edit policy) is a table:
//
//     Immediate              applied to running instances directly
//     ReinitializeComponent  the component is torn down and rebuilt from new data
//     RecreateEntity         the entity is recreated, preserving identity
//     ReloadAsset            the referenced asset is reloaded and rebound
//     RestartWorld           requires ending and restarting play
//     Unsupported            cannot be live edited; the change applies on the next run
//
// with two clauses that are the whole of the design: *"Policy SHALL be derivable by default from
// the field's classification and the component's nature, and overridable per field"*, and *"The
// editor SHALL report the applicable policy **before** the user acts, so that a change requiring a
// restart is known in advance rather than discovered."*
//
// Before M11.b, `grep -rniI 'LiveEditPolicy|ReinitializeComponent|RecreateEntity|RestartWorld'
// src/ editor/ tools/` returned nothing. The classification the defaults derive from did exist —
// `reflect::PersistenceKind{Authoring, RuntimeState, PersistentState, Derived}` — and the
// specification says so in its own purpose section: the classification *"until now had no
// consumer"*. This file is that consumer.
//
// ================================================================================================
// WHY THE DEFAULT IS DERIVED AND THE INTERESTING ANSWERS ARE NOT
// ================================================================================================
//
// Only three policies are ever DERIVED — `Immediate`, `ReinitializeComponent` and `Unsupported` —
// because those are the three the classification alone can justify:
//
//   Authoring, a plain value       -> Immediate.  The field is defined by the asset and the running
//                                    instance is supposed to follow it.
//   Authoring, an asset reference  -> ReloadAsset. Changing which asset is referenced is a rebind,
//                                    not a value write.
//   Derived                        -> ReinitializeComponent. The specification says derived fields
//                                    are recomputed; rebuilding the component from new data is what
//                                    recomputing one costs.
//   RuntimeState, PersistentState  -> Unsupported. The simulation owns the value. An authoring edit
//                                    of one is not a change the running world can be asked to make,
//                                    and "the change applies on the next run" is the truth about
//                                    it.
//
// `RecreateEntity` and `RestartWorld` are never derived. Nothing about a field's classification can
// tell you that changing it invalidates the entity or the world — that is knowledge about what the
// engine does with the value, and it has to be DECLARED. `LiveEditDecision::declared` carries which
// happened, and `live-edit-applies-without-a-restart` (m11b.toml) is written against exactly that
// distinction: *"the claim is that the policy is DECLARED per field rather than inferred"*.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/attributes.h>

#include <string_view>

namespace cy::gameplay::live {

/// What a change to a field means for a running world. `live-editing`'s own six, in order of how
/// much of the running world they disturb.
enum class LiveEditPolicy : u8 {
    /// Applied to running instances directly.
    Immediate = 0,
    /// The component is torn down and rebuilt from new data.
    ReinitializeComponent,
    /// The entity is recreated, preserving identity.
    RecreateEntity,
    /// The referenced asset is reloaded and rebound.
    ReloadAsset,
    /// Requires ending and restarting play.
    RestartWorld,
    /// Cannot be live edited; the change applies on the next run.
    Unsupported,
};

inline constexpr u32 kLiveEditPolicyCount = 6;

/// The policy's own spelling, for a diagnostic and for the editor's announcement. Never null.
[[nodiscard]] const char* live_edit_policy_name(LiveEditPolicy policy) noexcept;

/// The policy a word names, or an error naming the word. A word rather than a number on any wire,
/// for the reason `PlayState` and `PlayMode` are words: a seventh policy added on one side must be
/// refused by name rather than read as the closest one.
[[nodiscard]] Expected<LiveEditPolicy, Error> live_edit_policy_of(std::string_view name) noexcept;

/// How much of the running world a policy disturbs, on a scale where a larger number subsumes a
/// smaller one.
///
/// A transaction that touches several fields is applied at the **strongest** policy any of them
/// declares — applying a restart-requiring change as five immediate writes and one restart would
/// run the immediate five twice. `Unsupported` is the top of the scale because a transaction
/// containing one unsupported field cannot be applied at all.
[[nodiscard]] u8 live_edit_disturbance(LiveEditPolicy policy) noexcept;

/// The stronger of two policies, by `live_edit_disturbance`.
[[nodiscard]] LiveEditPolicy live_edit_stronger(LiveEditPolicy left, LiveEditPolicy right) noexcept;

/// What a field is, as far as a default policy is concerned.
///
/// Two of the three come straight from the reflected attributes the engine already carries; the
/// third is a fact about the field's TYPE rather than its classification, and is the one thing a
/// declaration site has to supply.
struct FieldNature {
    /// `serialization-and-prefabs`' classification, which is where the default comes from.
    reflect::PersistenceKind persistence = reflect::PersistenceKind::Authoring;
    /// Marked transient: never serialised, and by the same argument never carried across a rebuild.
    bool transient = false;
    /// The field names an asset rather than holding a value.
    bool asset_reference = false;
};

/// A policy and the provenance of the answer.
struct LiveEditDecision {
    LiveEditPolicy policy = LiveEditPolicy::Unsupported;
    /// True when a declaration named this policy for this field; false when it was derived from the
    /// classification. The editor shows the difference, and so does every test of this file: a
    /// policy that is always derived is not "per field".
    bool declared = false;
    /// Why, in one line, for the announcement the specification requires **before** the user acts.
    /// Never null; a string literal, because `cy::Error` and every diagnostic path here hold
    /// `const char*`.
    const char* reason = "";
};

/// The policy a field's classification implies, with no declaration in play.
[[nodiscard]] LiveEditDecision derived_policy_for(const FieldNature& nature) noexcept;

/// Per-field declarations, over the derived defaults.
///
/// Keyed by the pair (component type name, field name), which is the identity an authoring change
/// carries — a `.cyworld` names a component by the type name in its own `type` section and a field
/// by the name in that section's `field` line, and the live bridge carries the same two words. A
/// key of reflected `TypeId` would be better and is not available for every component this engine
/// simulates: physics' eight are registered by name with no reflected type behind them, which
/// `cy/gameplay/play/session.h` explains at length.
class LiveEditPolicyTable {
public:
    explicit LiveEditPolicyTable(Allocator& allocator) noexcept;

    /// Declare the policy for one field. Re-declaring the same field replaces the policy rather
    /// than failing: a project's declaration overriding the engine's is the point of the table.
    [[nodiscard]] Status declare(std::string_view type, std::string_view field,
                                 LiveEditPolicy policy) noexcept;

    /// The policy in force for one field: the declaration when there is one, and the classification
    /// default otherwise.
    ///
    /// This is the **announcement**. It answers without touching a running world, which is what
    /// makes *"the editor SHALL report the applicable policy before the user acts"* possible rather
    /// than a thing discovered by applying.
    [[nodiscard]] LiveEditDecision policy_for(std::string_view type, std::string_view field,
                                              const FieldNature& nature) const noexcept;

    [[nodiscard]] usize declarations() const noexcept { return entries_.size(); }

private:
    /// One declaration. The two names are COPIED rather than referenced — a table outlives the
    /// `string_view`s a caller built it from — and they are copied into one pool rather than into
    /// two arrays per entry, so an entry stays trivially copyable and the table stays one
    /// allocation per growth.
    struct Entry {
        u32 type_offset = 0;
        u32 type_length = 0;
        u32 field_offset = 0;
        u32 field_length = 0;
        LiveEditPolicy policy = LiveEditPolicy::Immediate;
    };

    [[nodiscard]] std::string_view text(u32 offset, u32 length) const noexcept;
    [[nodiscard]] const Entry* find(std::string_view type, std::string_view field) const noexcept;

    Array<Entry> entries_;
    Array<char> names_;
};

}  // namespace cy::gameplay::live
