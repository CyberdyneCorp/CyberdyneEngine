// The live edit policy table. See cy/gameplay/live/policy.h for why only three of the six policies
// are ever derived and the other three have to be declared.

#include <cy/gameplay/live/policy.h>

namespace cy::gameplay::live {
namespace {

/// The spellings, in enumerator order. The names are the specification's own, lower-cased with
/// hyphens, so a reader of a wire trace and a reader of the table are reading the same words.
constexpr std::string_view kNames[kLiveEditPolicyCount] = {
    "immediate",    "reinitialize-component", "recreate-entity",
    "reload-asset", "restart-world",          "unsupported",
};

/// Why a derived answer is what it is. Literals, because `LiveEditDecision::reason` outlives the
/// call that produced it.
constexpr const char* kWhyImmediate =
    "authoring field: the asset defines it and the running instance follows";
constexpr const char* kWhyAssetRebind =
    "authoring field naming an asset: a change to it is a rebind, not a value write";
constexpr const char* kWhyDerived =
    "derived field: it is recomputed rather than carried, so the component is rebuilt";
constexpr const char* kWhyRuntimeState =
    "runtime state: the simulation owns this value, so an authoring edit of it applies on the "
    "next run";
constexpr const char* kWhyPersistentState =
    "persistent state: the simulation owns this value and the overlay keeps it, so an authoring "
    "edit of it applies on the next run";
constexpr const char* kWhyTransient =
    "transient field: nothing carries it across a rebuild, so an authoring edit of it applies on "
    "the next run";
constexpr const char* kWhyDeclared = "declared for this field";

}  // namespace

const char* live_edit_policy_name(LiveEditPolicy policy) noexcept {
    const auto index = static_cast<u32>(policy);
    if (index >= kLiveEditPolicyCount) {
        return "unknown";
    }
    return kNames[index].data();
}

Expected<LiveEditPolicy, Error> live_edit_policy_of(std::string_view name) noexcept {
    for (u32 index = 0; index < kLiveEditPolicyCount; ++index) {
        if (kNames[index] == name) {
            return static_cast<LiveEditPolicy>(index);
        }
    }
    return fail(ErrorCode::InvalidArgument,
                "live edit policy: the named policy is not one this build knows — it is one of "
                "immediate, reinitialize-component, recreate-entity, reload-asset, restart-world "
                "or unsupported");
}

u8 live_edit_disturbance(LiveEditPolicy policy) noexcept {
    switch (policy) {
        case LiveEditPolicy::Immediate:
            return 0;
        case LiveEditPolicy::ReloadAsset:
            // Above Immediate and below a component rebuild: a rebind replaces what a field points
            // at without disturbing the component holding the field.
            return 1;
        case LiveEditPolicy::ReinitializeComponent:
            return 2;
        case LiveEditPolicy::RecreateEntity:
            return 3;
        case LiveEditPolicy::RestartWorld:
            return 4;
        case LiveEditPolicy::Unsupported:
            // The top, because a transaction containing one unsupported field cannot be applied at
            // all — not because it is "worse than a restart" in any other sense.
            return 5;
    }
    return 5;
}

LiveEditPolicy live_edit_stronger(LiveEditPolicy left, LiveEditPolicy right) noexcept {
    return live_edit_disturbance(left) >= live_edit_disturbance(right) ? left : right;
}

LiveEditDecision derived_policy_for(const FieldNature& nature) noexcept {
    LiveEditDecision decision;
    decision.declared = false;

    // Transient first, and before the classification, because a transient field is one nothing
    // carries anywhere — its classification says what it would mean if it were kept, and it is not.
    if (nature.transient) {
        decision.policy = LiveEditPolicy::Unsupported;
        decision.reason = kWhyTransient;
        return decision;
    }

    switch (nature.persistence) {
        case reflect::PersistenceKind::Authoring:
            if (nature.asset_reference) {
                decision.policy = LiveEditPolicy::ReloadAsset;
                decision.reason = kWhyAssetRebind;
                return decision;
            }
            decision.policy = LiveEditPolicy::Immediate;
            decision.reason = kWhyImmediate;
            return decision;

        case reflect::PersistenceKind::Derived:
            decision.policy = LiveEditPolicy::ReinitializeComponent;
            decision.reason = kWhyDerived;
            return decision;

        case reflect::PersistenceKind::RuntimeState:
            decision.policy = LiveEditPolicy::Unsupported;
            decision.reason = kWhyRuntimeState;
            return decision;

        case reflect::PersistenceKind::PersistentState:
            decision.policy = LiveEditPolicy::Unsupported;
            decision.reason = kWhyPersistentState;
            return decision;
    }

    decision.policy = LiveEditPolicy::Unsupported;
    decision.reason = kWhyRuntimeState;
    return decision;
}

// --- The table -----------------------------------------------------------------------------------

LiveEditPolicyTable::LiveEditPolicyTable(Allocator& allocator) noexcept
    : entries_(allocator), names_(allocator) {}

std::string_view LiveEditPolicyTable::text(u32 offset, u32 length) const noexcept {
    if (static_cast<usize>(offset) + length > names_.size()) {
        return {};
    }
    return {names_.data() + offset, length};
}

const LiveEditPolicyTable::Entry* LiveEditPolicyTable::find(std::string_view type,
                                                            std::string_view field) const noexcept {
    for (const Entry& entry : entries_) {
        if (text(entry.type_offset, entry.type_length) == type &&
            text(entry.field_offset, entry.field_length) == field) {
            return &entry;
        }
    }
    return nullptr;
}

Status LiveEditPolicyTable::declare(std::string_view type, std::string_view field,
                                    LiveEditPolicy policy) noexcept {
    if (type.empty() || field.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "live edit policy: a declaration names a component type and a field, and "
                    "neither may be empty");
    }
    // A re-declaration REPLACES rather than fails: a project overriding the engine's default for a
    // field is the point of the table, and refusing the second declaration would make the order two
    // modules registered in load-bearing.
    for (Entry& entry : entries_) {
        if (text(entry.type_offset, entry.type_length) == type &&
            text(entry.field_offset, entry.field_length) == field) {
            entry.policy = policy;
            return ok();
        }
    }

    Entry entry;
    entry.policy = policy;
    entry.type_offset = static_cast<u32>(names_.size());
    entry.type_length = static_cast<u32>(type.size());
    if (Status appended = names_.append(Span<const char>(type.data(), type.size())); !appended) {
        return appended;
    }
    entry.field_offset = static_cast<u32>(names_.size());
    entry.field_length = static_cast<u32>(field.size());
    if (Status appended = names_.append(Span<const char>(field.data(), field.size())); !appended) {
        return appended;
    }
    return entries_.push_back(entry);
}

LiveEditDecision LiveEditPolicyTable::policy_for(std::string_view type, std::string_view field,
                                                 const FieldNature& nature) const noexcept {
    if (const Entry* entry = find(type, field); entry != nullptr) {
        LiveEditDecision decision;
        decision.policy = entry->policy;
        decision.declared = true;
        decision.reason = kWhyDeclared;
        return decision;
    }
    return derived_policy_for(nature);
}

}  // namespace cy::gameplay::live
