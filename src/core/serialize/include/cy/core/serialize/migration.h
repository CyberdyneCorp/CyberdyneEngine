#pragma once
// Schema versions, and migration on the value record. Task 3.2.9.
//
// `serialization-and-prefabs` — "Schema versioning and migration" and "Value-level migration" —
// fixes three things this header implements and one it rules out.
//
//   * A schema version is **independent of type identity**: a `TypeId` never changes, and the
//     schema version advances when the data's meaning or shape does. A rename advances nothing,
//     because identity already survived it.
//   * A migration operates on the **value record** and "SHALL NOT require constructing an instance
//     of an older version of the type, which no longer exists in the code". So a migration function
//     takes a `ValueRecord&` and no object of any kind.
//   * Migration applies to **every form of tagged data addressed by these identifiers**: assets,
//     scenes, prefabs, prefab overrides, and saves. "A migration that updates an asset but drops
//     the overrides on it SHALL be a defect." That is why `FieldRemap` is part of a migration
//     rather than something a custom function does privately: an override addresses a field by
//     identifier and has no value record to run a function over, so the identifier change has to be
//     declarative for the override migrator to be able to apply the same one.
//
// What is ruled out is a migration that needs the old type. There is no hook that hands one a
// `void*`, because the moment such a hook exists the first migration written against it pins a
// struct definition into the codebase forever.
//
// TWO FAILURES ARE HARD, AND BOTH NAME THE VERSIONS.
//
//   data older than the oldest migration    the chain cannot be walked, so the load fails rather
//                                           than producing partially initialised data.
//   data newer than this build              the fields mean something this build does not know, so
//                                           the load fails rather than misinterpreting them.
//
// A type nobody declared is treated as version-zero-and-current: an unregistered type's records
// pass through untouched, which is what keeps unknown data preserved rather than rejected.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/serialize/value_record.h>

namespace cy::serialize {

/// Who writes a migration, from the specification's table. Recorded so a cook report can say how
/// much of a chain was hand-written, which is the number that predicts where the bugs are.
enum class MigrationClass : u8 {
    /// Rename, add with default, remove, safe numeric widening. Identity handles it; no function.
    Automatic = 0,
    /// Enum value remap, container kind change, wrapping in an optional. The generator emits it.
    Generated = 1,
    /// A field split, a unit change, a semantic change. A developer writes it.
    Custom = 2,
};

const char* migration_class_name(MigrationClass value) noexcept;

/// A field identity change, applied to data and to the overrides that address it.
struct FieldRemap {
    reflect::FieldId from;
    reflect::FieldId to;
};

/// A migration operates on a value record and nothing else. `context` is whatever the caller passed
/// to `migrate()`; it exists so a project's migrations can reach a lookup table without a global.
using MigrationFn = Status (*)(ValueRecord& record, void* context) noexcept;

/// One step of a chain: from one version to the next.
struct Migration {
    reflect::TypeId type;
    u16 from_version = 0;
    u16 to_version = 0;
    MigrationClass kind = MigrationClass::Automatic;
    /// For the diagnostic and the cook report. Never null in a registered migration.
    const char* name = "";
    /// Null for a migration that is only a set of remaps, which is the common automatic case.
    MigrationFn apply = nullptr;
    /// Identifier changes this step makes. Copied into the registry, so a caller may build them on
    /// the stack.
    Span<const FieldRemap> remaps;
};

/// The declared schema version of every type, and the chain that reaches it.
class SchemaRegistry {
public:
    explicit SchemaRegistry(Allocator& allocator = current_allocator()) noexcept
        : types_(allocator), steps_(allocator), remaps_(allocator) {}

    /// Declare a type's current schema version. Re-declaring the same version is a no-op;
    /// re-declaring a different one is refused, because two answers to "what version is current"
    /// is the condition under which half the data in a project migrates and half does not.
    [[nodiscard]] Status declare(reflect::TypeId type, u16 current_version) noexcept;

    /// Add one step. Refused unless it advances by exactly one version: a chain with a gap cannot
    /// be walked, and a chain that skips a version silently loses whatever that version did.
    [[nodiscard]] Status add_migration(const Migration& migration) noexcept;

    /// The declared current version, or nothing when the type was never declared.
    [[nodiscard]] Expected<u16, Error> current_version(reflect::TypeId type) const noexcept;
    [[nodiscard]] bool declares(reflect::TypeId type) const noexcept;

    /// Walk `record` up to its type's current version.
    ///
    /// A record of an undeclared type is left alone and reported as success — that is the unknown
    /// component case, and rejecting it would strip a disabled plugin's data.
    [[nodiscard]] Status migrate(ValueRecord& record, void* context = nullptr) const noexcept;

    /// Walk one override's field identifier up to the current version.
    ///
    /// The same chain, applied to a target rather than to data. `field` is updated in place, and
    /// `from_version` is the version the override was authored against. This is the call that makes
    /// "overrides SHALL be migrated to the new identifiers, and SHALL NOT be discarded" true.
    [[nodiscard]] Status migrate_field_id(reflect::TypeId type, u16 from_version,
                                          reflect::FieldId& field) const noexcept;

    [[nodiscard]] usize migration_count() const noexcept { return steps_.size(); }

private:
    struct Declaration {
        reflect::TypeId type;
        u16 current_version = 0;
    };

    /// A registered step. The remaps are a range of the registry's own pool, so the caller's
    /// storage does not have to outlive the call.
    struct Step {
        reflect::TypeId type;
        u16 from_version = 0;
        u16 to_version = 0;
        MigrationClass kind = MigrationClass::Automatic;
        const char* name = "";
        MigrationFn apply = nullptr;
        u32 remap_offset = 0;
        u32 remap_count = 0;
    };

    [[nodiscard]] const Declaration* find(reflect::TypeId type) const noexcept;
    [[nodiscard]] const Step* find_step(reflect::TypeId type, u16 from_version) const noexcept;

    Array<Declaration> types_;
    Array<Step> steps_;
    Array<FieldRemap> remaps_;
};

}  // namespace cy::serialize
