// The reflected type set M1 owns. Sections 1.1 and 1.2.
//
// Reflection needs something reflected to be provable, and at M1 there is nothing else: components
// arrive with the ECS at M2, resources with the renderer at M3. These two structs are that
// something — a real, committed, annotated corpus that exercises every field attribute in
// `core-type-system`'s table, holds the first identifiers the manifest ever assigned, and is what
// the identity gate is proved against by renaming one of its fields.
//
// They are demonstrations, not engine vocabulary: nothing in the engine stores a cy::demo::Health.
// When M2's components land they join this corpus rather than replacing it, because the identifiers
// already assigned here are the oldest entries in the manifest and deleting them would be the first
// test of the tombstone rule for no reason.
//
// This header includes annotations.h and the fixed-width types and nothing else, on purpose. Every
// include here is re-parsed by the generator for every build that touches it, and the M1 spike
// measured a ninefold increase in cold generation time from two standard-library includes per
// header at an identical type count. A reflected header should be able to say what it costs.

#ifndef CY_CORE_REFLECT_DEMO_TYPES_H
#define CY_CORE_REFLECT_DEMO_TYPES_H

#include <cy/core/base/types.h>
#include <cy/core/reflect/annotations.h>

namespace cy::demo {

/// What last hurt an entity. The persistent values are what serialized data carries, so reordering
/// the enumerators is a rename and not a renumbering.
enum class DamageKind : u8 {
    Physical = 0,
    Fire = 1,
    Frost = 2,
    Arcane = 3,
};

/// Hit points, and the presentation state around them.
///
/// The split between `maximum` and `current` is the one `serialization-and-prefabs` uses to explain
/// field classification: editing the prefab updates the maximum and preserves the current value,
/// because one is Authoring and the other is RuntimeState.
struct CY_REFLECT_TYPE(Category("Gameplay"), Tooltip("Hit points and their presentation")) Health {
    CY_REFLECT_FIELD(Range(0.0, 10000.0, 1.0), Unit(Percent), Category("Combat"),
                     Tooltip("The value healing cannot exceed"), Persistence(Authoring))
    f32 maximum = 100.0F;

    CY_REFLECT_FIELD(Range(0.0, 10000.0), Category("Combat"), Persistence(RuntimeState),
                     Replicated("quantised", "bits=16", "owner-and-observers"))
    f32 current = 100.0F;

    /// A per-frame smoothing of `current` for the health bar. Rebuilt every frame, so it is
    /// Transient: excluded from serialization and from replication, and never in a record.
    CY_REFLECT_FIELD(Transient, Hidden, Persistence(Derived))
    f32 displayed = 100.0F;

    CY_REFLECT_FIELD(Enum(Physical = 0, Fire = 1, Frost = 2, Arcane = 3), ReadOnly,
                     Category("Combat"), Persistence(RuntimeState))
    u8 last_damage = 0;

    /// The icon shown beside the bar. A u64 rather than an AssetId because AssetId is the values
    /// module's type and lands at task 1.3.2; the AssetRef attribute already says what it means, so
    /// the inspector offers a texture picker without this module knowing what a texture is.
    CY_REFLECT_FIELD(AssetRef("Texture"), Category("Presentation"))
    u64 icon = 0;
};

/// Placement on a plane. Enough to exercise units, flags, and a module's own attribute.
struct CY_REFLECT_TYPE(Category("Spatial")) Placement {
    CY_REFLECT_FIELD(Unit(Metres), Category("Transform"))
    f32 x = 0.0F;

    CY_REFLECT_FIELD(Unit(Metres), Category("Transform"))
    f32 y = 0.0F;

    CY_REFLECT_FIELD(Unit(Radians), Range(-3.14159265, 3.14159265), Category("Transform"))
    f32 rotation = 0.0F;

    CY_REFLECT_FIELD(Flags(None = 0, Static = 1, Frozen = 2, Occluder = 4), Category("Transform"))
    u32 flags = 0;

    /// `Streaming` is not in the specification's table. It is declared by this module in
    /// reflect_attributes.toml and emitted as a typed struct exactly like the built-in ones — which
    /// is what `core-type-system` requires of a project's own attribute.
    CY_REFLECT_FIELD(Streaming(priority = 2, prefetch = true), Category("Streaming"),
                     Persistence(Authoring))
    u32 tile = 0;
};

}  // namespace cy::demo

#endif  // CY_CORE_REFLECT_DEMO_TYPES_H
