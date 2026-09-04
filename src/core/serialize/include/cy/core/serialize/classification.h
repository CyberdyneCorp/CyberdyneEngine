#pragma once
// Field classification: the one declaration serialization, live update, persistence and networking
// all read. Task 3.2.3.
//
// `serialization-and-prefabs` — "Field classification" — gives four classes and requires that they
// be "part of the type's reflected schema, so that serialization, live update, persistence, and
// networking all derive their behaviour from one declaration". M1 already put the declaration in
// the schema: it is `reflect::PersistenceKind`, carried on every `FieldAttributes`.
//
// What was missing is the derivation. Nothing turned a class into a decision, so every consumer
// would have written its own table and the four would have drifted. This header is that table, and
// it is the only one: a caller says what it is writing *for*, and gets back which fields belong in
// it.
//
//   Purpose             Authoring  RuntimeState  PersistentState  Derived
//   Asset                  yes         no             no             no
//   Persistence            no          no             yes            no
//   Snapshot               yes         yes            yes            no
//   Replication            per the field's Replicated attribute, which is a different declaration
//
// `Derived` is absent from every column, which is what "never serialised; recomputed on load" means
// when it is a rule rather than a sentence. `Transient` — a separate attribute, not a class — is
// absent from every column too.
//
// WHY `Snapshot` INCLUDES EVERYTHING. A replay or a play-mode reset must reproduce the simulation
// exactly, and a `RuntimeState` field is part of the simulation. The class says the field is not
// *persisted* — it does not survive an asset update, and it is not written to the save overlay —
// and that is a different question from whether a verbatim capture of the running world contains
// it. Conflating the two would make a snapshot restore silently lose the current health of every
// robot in the world.

#include <cy/core/base/types.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>

namespace cy::serialize {

/// What a traversal is producing. The one input a caller gives the classification table.
enum class Purpose : u8 {
    /// The authoring artefact: a prefab, a scene, a world chunk under version control.
    Asset = 0,
    /// The persistence overlay: what a save writes over the cooked world.
    Persistence = 1,
    /// A verbatim capture of live state: a snapshot, a replay frame, a play-mode reset.
    Snapshot = 2,
};

const char* purpose_name(Purpose purpose) noexcept;

/// True when a field of this class belongs in data written for this purpose. The table above, as
/// code, and the only copy of it.
[[nodiscard]] constexpr bool class_is_written(reflect::PersistenceKind kind,
                                              Purpose purpose) noexcept {
    switch (kind) {
        case reflect::PersistenceKind::Derived:
            return false;
        case reflect::PersistenceKind::Authoring:
            return purpose == Purpose::Asset || purpose == Purpose::Snapshot;
        case reflect::PersistenceKind::RuntimeState:
            return purpose == Purpose::Snapshot;
        case reflect::PersistenceKind::PersistentState:
            return purpose == Purpose::Persistence || purpose == Purpose::Snapshot;
    }
    return false;
}

/// True when this field belongs in data written for this purpose.
///
/// Two rules, in order: `Transient` is excluded from everything — `core-type-system` requires it
/// excluded from serialization and from replication, and the writer is where that is cheapest to
/// enforce — and then the class table decides.
[[nodiscard]] constexpr bool field_is_written(const reflect::FieldInfo& field,
                                              Purpose purpose) noexcept {
    if (field.attributes.transient()) {
        return false;
    }
    return class_is_written(field.attributes.persistence, purpose);
}

/// True when a field of this class keeps its running value when the asset it came from is updated
/// underneath it. The live-update half of the same declaration: `serialization-and-prefabs`'
/// "Health.max is Authoring and Health.current is RuntimeState" scenario is this predicate.
[[nodiscard]] constexpr bool class_survives_asset_update(reflect::PersistenceKind kind) noexcept {
    return kind == reflect::PersistenceKind::RuntimeState ||
           kind == reflect::PersistenceKind::PersistentState;
}

/// True when a field of this class is recomputed rather than carried across a live update.
[[nodiscard]] constexpr bool class_is_recomputed(reflect::PersistenceKind kind) noexcept {
    return kind == reflect::PersistenceKind::Derived;
}

}  // namespace cy::serialize
