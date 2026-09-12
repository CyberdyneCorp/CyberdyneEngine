#pragma once
// The interaction field: moving things bend foliage, with no physics body per plant. M10 task 2.4.
//
// `foliage` — "Interaction field": "Foliage SHALL respond to moving objects through a GPU
// INTERACTION FIELD: characters, vehicles, and projectiles register interaction primitives that
// bend, flatten, or displace foliage nearby. Interaction SHALL NOT REQUIRE PHYSICS BODIES PER
// PLANT. The field SHALL have a BOUNDED EXTENT around streaming sources and a BOUNDED NUMBER OF
// CONTRIBUTORS, with the LOWEST-PRIORITY CONTRIBUTORS DROPPED DETERMINISTICALLY. Persistent
// flattening — a trail through grass — SHALL be supported as a DECAYING CONTRIBUTION with a
// DECLARED LIFETIME."
//
// ================================================================================================
// "DROPPED DETERMINISTICALLY" IS THE SENTENCE THAT DECIDES THE DATA STRUCTURE
// ================================================================================================
//
// A contributor list that dropped by arrival order would drop a different primitive on a machine
// whose frame boundaries fell elsewhere, and the two machines' grass would differ — which matters
// because the interaction field is what a `Persistent` trail is deposited from, and a trail is
// saved.
//
// So the admission is a SORT, not a queue: contributors are ordered by (priority, then a stable
// identity, then the primitive's own quantised position) and the tail beyond the bound is dropped.
// `register_primitive()` may be called in any order and `resolve()` gives the same set. The
// identity is the CALLER's — an entity index, a projectile id — because an identity this module
// invented from a counter would be the traversal counter the M10 spike condemned.
//
// ================================================================================================
// THE TRAIL IS A GRID, THE CONTRIBUTORS ARE A LIST, AND THEY ARE DIFFERENT THINGS
// ================================================================================================
//
// An instantaneous bend is a query against the live contributors: it must be exact, it must follow
// a vehicle at 30 m/s, and it costs nothing to store. A TRAIL is state — it outlives the character
// that made it, decays over a declared lifetime and is what "a flattened trail SHALL persist" means
// — so it is a fixed grid that shifts in whole cells around the streaming source, exactly the shape
// `water::FoamGrid` uses and for the same reason: its bytes are a function of the resolution alone,
// so a trail that crosses a continent costs what a trail that crosses a field costs.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/world/coordinates.h>

namespace cy::foliage {

/// The shapes a contributor may have. Three, because a character is a capsule, a blast is a sphere
/// and a vehicle is a box, and a fourth would be a shape nothing registers.
enum class InteractionShape : u8 { Sphere = 0, Capsule, Box };

[[nodiscard]] const char* interaction_shape_name(InteractionShape shape) noexcept;

/// What a contributor does to the foliage it reaches.
enum class InteractionEffect : u8 {
    /// Pushed aside, springing back. A character walking.
    Bend = 0,
    /// Pressed down and staying down for a while. A vehicle, a landing.
    Flatten,
    /// Pushed outward from the primitive. A blast.
    Displace,
};

[[nodiscard]] const char* interaction_effect_name(InteractionEffect effect) noexcept;

/// One registered contributor.
struct InteractionPrimitive {
    /// The CALLER's stable identity for whatever is interacting. See the header note: the drop
    /// order depends on it, so it must not be a counter this module invented.
    u64 source = 0;
    InteractionShape shape = InteractionShape::Sphere;
    InteractionEffect effect = InteractionEffect::Bend;
    /// Absolute position. For a capsule it is one end; `extent` is the other, relative.
    world::WorldVec3d position;
    /// The capsule's axis or the box's half-extents, in metres.
    Vec3 extent{0.0F, 0.0F, 0.0F};
    f32 radius_metres = 0.5F;
    /// 0..1. How hard the foliage is pushed.
    f32 strength = 1.0F;
    /// Higher survives the bound. `foliage` — "the lowest-priority contributors dropped".
    u32 priority = 0;
    /// Seconds this contributor lives. Zero means "this frame only", which is the ordinary case for
    /// a moving character; a positive value is what a lingering blast declares.
    f32 lifetime_seconds = 0.0F;
    /// Whether this contributor also deposits into the persistent trail grid.
    bool deposits_trail = false;
};

/// What the field may cost. `foliage` — "a BOUNDED EXTENT around streaming sources and a BOUNDED
/// NUMBER OF CONTRIBUTORS", and the budget's "interaction field resolution" lever.
struct InteractionBounds {
    /// Half the side of the square the field covers, in metres, centred on the streaming source.
    f32 extent_metres = 64.0F;
    /// The largest number of contributors that may be active at once.
    u32 max_contributors = 64;
    /// Trail grid cells per edge. The resolution lever; the grid's bytes are this squared.
    u32 trail_cells = 128;
    /// Seconds a deposited trail takes to decay to 1/e of its strength.
    f32 trail_lifetime_seconds = 30.0F;

    [[nodiscard]] bool is_valid() const noexcept {
        return extent_metres > 0.0F && max_contributors > 0 && trail_cells > 0 &&
               trail_lifetime_seconds > 0.0F;
    }
    /// Metres per trail cell.
    [[nodiscard]] f32 trail_cell_metres() const noexcept {
        return (extent_metres * 2.0F) / static_cast<f32>(trail_cells);
    }
    /// Bytes the trail grid occupies. A function of `trail_cells` alone, which is the whole point.
    [[nodiscard]] u64 trail_bytes() const noexcept {
        return static_cast<u64>(trail_cells) * static_cast<u64>(trail_cells);
    }
};

/// What one resolve admitted and dropped.
struct InteractionReport {
    u32 registered = 0;
    u32 admitted = 0;
    /// Dropped because the bound was reached. Deterministic; see the header note.
    u32 dropped_priority = 0;
    /// Dropped because they fell outside the field's extent.
    u32 dropped_extent = 0;
    /// Dropped because their lifetime expired.
    u32 dropped_expired = 0;
};

/// The bend one position receives, and where it came from.
struct InteractionSample {
    /// Horizontal displacement in metres. +Y is never written: foliage bends, it does not levitate.
    Vec3 offset{0.0F, 0.0F, 0.0F};
    /// 0..1 how flattened the foliage is here, live contributors and trail together.
    f32 flatten = 0.0F;
    /// How many live contributors reached this position.
    u32 contributors = 0;
    /// The trail's own contribution to `flatten`, separated so a diagnostic can tell a vehicle
    /// standing still from the track it left.
    f32 trail = 0.0F;
};

/// The field. Holds the contributors, the trail grid, and the sampling both grass and instanced
/// foliage do through it.
class InteractionField {
public:
    InteractionField(Allocator& allocator, const InteractionBounds& bounds) noexcept;

    InteractionField(const InteractionField&) = delete;
    InteractionField& operator=(const InteractionField&) = delete;

    [[nodiscard]] const InteractionBounds& bounds() const noexcept { return bounds_; }
    /// Change the resolution. Reallocates and CLEARS the trail: a trail resampled onto a different
    /// grid is a trail that moved, and a budget lever that moved the world's tracks would be worse
    /// than one that forgot them.
    [[nodiscard]] Status set_bounds(const InteractionBounds& bounds) noexcept;

    /// Move the field's centre. Shifts the trail grid by WHOLE CELLS, so a track stays where it was
    /// laid; the cells that scroll in are cleared.
    [[nodiscard]] Status recentre(const world::WorldVec3d& centre) noexcept;
    [[nodiscard]] const world::WorldVec3d& centre() const noexcept { return centre_; }

    /// Register a contributor for this frame. Order-independent: `resolve()` sorts.
    [[nodiscard]] Status register_primitive(const InteractionPrimitive& primitive) noexcept;

    /// Admit the highest-priority contributors up to the bound, deposit trails, and decay the grid
    /// by `seconds`. Clears the registration list.
    [[nodiscard]] Expected<InteractionReport, Error> resolve(f32 seconds) noexcept;

    /// The bend at a position. What a grass blade and an instanced plant both call.
    [[nodiscard]] InteractionSample sample(const world::WorldVec3d& at) const noexcept;

    [[nodiscard]] Span<const InteractionPrimitive> admitted() const noexcept {
        return admitted_.span();
    }
    [[nodiscard]] usize pending() const noexcept { return pending_.size(); }
    [[nodiscard]] u64 bytes() const noexcept;
    /// The strongest trail value anywhere. A test's handle on "the trail decayed".
    [[nodiscard]] f32 peak_trail() const noexcept;

private:
    struct Active {
        InteractionPrimitive primitive;
        /// Seconds remaining. Zero-lifetime contributors are dropped at the next resolve.
        f32 remaining = 0.0F;
    };

    [[nodiscard]] Status deposit_trail(const InteractionPrimitive& primitive) noexcept;
    [[nodiscard]] bool cell_of(const world::WorldVec3d& at, i32& cx, i32& cz) const noexcept;

    Allocator* allocator_;
    InteractionBounds bounds_;
    world::WorldVec3d centre_;
    /// Where the grid's origin cell sits, in absolute cell units. Shifting moves this, not the
    /// bytes — the same arrangement `water::FoamGrid` uses.
    i64 origin_cell_x_ = 0;
    i64 origin_cell_z_ = 0;
    Array<InteractionPrimitive> pending_;
    Array<Active> active_;
    Array<InteractionPrimitive> admitted_;
    /// UNorm8 flattening, one byte a cell.
    Array<u8> trail_;
};

}  // namespace cy::foliage
