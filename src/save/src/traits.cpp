// Persistence traits, read out of the declaration that already exists. Task 6.1.

#include <cy/save/traits.h>

namespace cy::save {

Trait traits_of(const reflect::FieldInfo& field) noexcept {
    // Transient is excluded from everything — from serialization and from replication — so it
    // carries no trait at all rather than a trait that happens to be written nowhere.
    if (field.attributes.transient()) {
        return Trait::None;
    }

    Trait traits = Trait::None;
    switch (field.attributes.persistence) {
        case reflect::PersistenceKind::Authoring:
            traits |= Trait::Authoring;
            break;
        case reflect::PersistenceKind::RuntimeState:
            traits |= Trait::RuntimeOnly;
            break;
        case reflect::PersistenceKind::PersistentState:
            traits |= Trait::SaveGame;
            break;
        case reflect::PersistenceKind::Derived:
            traits |= Trait::Derived;
            break;
    }

    // Replay relevance is the Snapshot column of the classification table, and it is a different
    // question from persistence: a `RuntimeState` field is not saved and is still part of a
    // verbatim capture of the running world.
    if (serialize::field_is_written(field, serialize::Purpose::Snapshot)) {
        traits |= Trait::ReplayRelevant;
    }
    if (field.attributes.declares(reflect::AttributeKind::Replicated)) {
        traits |= Trait::Replicated;
    }
    if (field_is_saved(field) && scope_of(field) == Scope::Profile) {
        traits |= Trait::Profile;
    }
    return traits;
}

Scope scope_of(const reflect::FieldInfo& field) noexcept {
    const auto* declared =
        reflect::find_custom<SaveScopeAttribute>(field.attributes, kSaveScopeAttribute);
    if (declared == nullptr || !is_known_scope(declared->scope)) {
        return Scope::World;
    }
    return static_cast<Scope>(declared->scope);
}

}  // namespace cy::save
