#pragma once
// CyberWater's vocabulary: what a water body IS, which backend simulates it, and which of several
// overlapping bodies owns a point. M10 task 2.3.
//
// `water` — "Water bodies": "Water SHALL be modelled as water bodies: an identity, a type, a
// material, a simulation backend, a mean level, bounds, and streaming metadata", and types "SHALL
// be one abstraction with different backends, not separate systems".
//
// --- WHY THE BACKEND IS A FIELD OF THE DESCRIPTION AND NOT A CLASS ------------------------------
//
// The failure this shape exists to prevent is four systems that happen to be wet: an ocean type
// with an ocean query, a river type with a river query, and a consumer that has to know which it is
// holding before it can ask how deep the water is. So the backend is a value on `WaterBodyDesc`,
// every consumer asks `WaterSystem::query()`, and "a lake configured to use a smaller spectral
// model than the ocean" is a different number in a description rather than a different code path in
// a caller — which is the specification's "Backend is a body property" scenario stated as a type.
//
// --- THE TWO BACKENDS THAT ARE NOT BUILT, AND WHY THEY ARE STILL ENUMERATED ---------------------
//
// The specification's backend table declares `ShallowWater` **Planned** and `Particle`
// **Deferred**, and the deferred one carries a further requirement: its seam "SHALL be the same
// seam `vfx-system` reserves for fluids; the two SHALL NOT become separate fluid systems". Both are
// enumerated here and both are REFUSED by `WaterRegistry::add()` with a message that says which of
// the two states it is in and, for `Particle`, where the seam is. An enumerator that a registry
// silently accepted would be a body that simulated as `Flat` and nobody would find out from the
// configuration; an enumerator that did not exist at all would let a project's description of a
// planned body fail to parse rather than fail to run.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/world/coordinates.h>

namespace cy::water {

// --- Identity -----------------------------------------------------------------------------------

/// A body's identity: a hash of its stable name, resolved at compile time where the name is a
/// literal.
///
/// An identity and not an index, for the reason `environment::FieldId` is one and for one more
/// that belongs to this capability: "A river SHALL NOT be split into unrelated per-cell objects;
/// its network and identity SHALL be GLOBAL while its runtime data is segmented." A body's identity
/// therefore cannot be a slot in a table of what is currently resident.
struct WaterBodyId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(WaterBodyId, WaterBodyId) noexcept = default;
};

namespace detail {

/// FNV-1a over a name. Unseeded and `constexpr`, for the reason `environment::detail::hash_name()`
/// gives: `cy::hash_bytes()` is randomised per process in development builds, and a body identity
/// that changed between two runs of a cooker would not be an identity. Save data and the network
/// protocol key on this number.
[[nodiscard]] constexpr u64 hash_name(const char* text) noexcept {
    u64 value = 0xcbf2'9ce4'8422'2325ULL;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        value ^= static_cast<u64>(static_cast<u8>(*cursor));
        value *= 0x0000'0100'0000'01b3ULL;
    }
    return value;
}

}  // namespace detail

[[nodiscard]] constexpr WaterBodyId water_body_id(const char* name) noexcept {
    return WaterBodyId{detail::hash_name(name)};
}

// --- Type and backend ---------------------------------------------------------------------------

/// `water` — "Types SHALL include at minimum: ocean, lake, river, pool, and waterfall."
enum class WaterBodyType : u8 { Ocean = 0, Lake, River, Pool, Waterfall };

[[nodiscard]] const char* water_body_type_name(WaterBodyType type) noexcept;

/// The specification's backend table, enumerated in its own order.
enum class WaterBackend : u8 { Flat = 0, Spectral, SplineFlow, ShallowWater, Particle };

[[nodiscard]] const char* water_backend_name(WaterBackend backend) noexcept;

/// What the specification's own Status column says about a backend. Carried as data so that the
/// registry's refusal can state which of the three it is refusing rather than listing enumerators.
enum class BackendStatus : u8 { Required = 0, Planned, Deferred };

[[nodiscard]] BackendStatus backend_status(WaterBackend backend) noexcept;
[[nodiscard]] const char* backend_status_name(BackendStatus status) noexcept;

// --- Optics ---------------------------------------------------------------------------------

/// How light behaves in this body's water. `water` — "Water surface shading": the closure "SHALL
/// account for ... wavelength-dependent ABSORPTION and SCATTERING through the water column", and
/// "Underwater rendering": the engine applies "THAT BODY'S PARAMETERS — absorption, scattering, and
/// colour — rather than a fixed fullscreen tint".
///
/// Per-channel and per-metre, because that is the unit the Beer-Lambert law takes and because
/// "deep water darkens and shifts hue without hand-authored gradients" is only true when the three
/// channels attenuate at different rates. Clear sea water absorbs red roughly thirty times faster
/// than blue, and that ratio is the whole of the effect.
struct WaterOptics {
    /// Absorption coefficient per metre, per channel.
    Vec3 absorption{0.45F, 0.09F, 0.02F};
    /// Scattering coefficient per metre, per channel.
    Vec3 scattering{0.003F, 0.005F, 0.010F};
    /// The colour scattered back out of the column — silt, plankton, glacial flour.
    Vec3 scatter_colour{0.10F, 0.35F, 0.45F};
    /// Surface microfacet roughness before wave normals are applied.
    f32 roughness = 0.02F;
    /// Index of refraction. 1.333 for water; a body may declare otherwise.
    f32 refractive_index = 1.333F;
};

/// The clear open sea, and a silty inland lake. Two bodies whose underwater appearance differs
/// because their parameters differ, which is the specification's own scenario and the reason these
/// are named constants rather than a single default with a comment.
[[nodiscard]] WaterOptics clear_sea_optics() noexcept;
[[nodiscard]] WaterOptics silty_lake_optics() noexcept;

// --- Bounds ---------------------------------------------------------------------------------

/// A body's extent, in absolute metres. f64 for the reason `world::WorldVec3d` is f64: a lake a
/// thousand kilometres out has 64 mm of f32 spacing and a bound that had to be widened to be safe
/// would decide body ownership differently at the far edge of a world than at its origin.
///
/// `min_y` is the deepest point the body may occupy and `max_y` the highest the surface may reach.
/// Both exist because "a camera or listener is INSIDE a water body" is a volume test, not a
/// comparison against a level.
struct WaterBounds {
    f64 min_x = 0.0;
    f64 min_y = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_y = 0.0;
    f64 max_z = 0.0;

    [[nodiscard]] bool valid() const noexcept {
        return max_x > min_x && max_y > min_y && max_z > min_z;
    }
    /// Horizontal containment. The test that decides which body owns a point on the surface.
    [[nodiscard]] bool contains_horizontal(const world::WorldVec3d& at) const noexcept {
        return at.x >= min_x && at.x <= max_x && at.z >= min_z && at.z <= max_z;
    }
    [[nodiscard]] bool contains(const world::WorldVec3d& at) const noexcept {
        return contains_horizontal(at) && at.y >= min_y && at.y <= max_y;
    }
};

// --- The description ------------------------------------------------------------------------

/// Everything a body declares. One structure for five types over five backends, which is the
/// capability's first requirement expressed as a type rather than as a class hierarchy.
struct WaterBodyDesc {
    /// A stable name. Its hash is the identity save data and the network protocol key on, so
    /// renaming a body is a data migration and not a cosmetic edit.
    const char* name = "";
    WaterBodyType type = WaterBodyType::Lake;
    WaterBackend backend = WaterBackend::Flat;

    /// The still-water level, in absolute metres. What a query returns where no simulation is
    /// resident — "A query in a region whose water data is not resident SHALL return the body's
    /// MEAN LEVEL with a resolution indicator."
    f64 mean_level = 0.0;
    WaterBounds bounds;

    /// The declared resolution order. `water` — "A position MAY be inside more than one [body],
    /// with a DECLARED resolution order." Higher wins; ties are broken by declaration order, which
    /// is stated in `WaterRegistry::body_at()` and tested, because "one consistent answer" is a
    /// property of the tie-break existing rather than of it being any particular rule.
    i32 priority = 0;

    /// Density, kg/m^3. Fresh water is 1000 and sea water about 1025; a body declares its own
    /// because buoyancy is computed from it and a boat floats measurably higher in the sea.
    f32 density = 1000.0F;

    WaterOptics optics;

    /// The material the surface is shaded with. An opaque handle: `material-compiler` owns what it
    /// means, and a water module that resolved it would be a water module that needed a renderer.
    u64 surface_material = 0;

    /// The edge of one streaming segment, in metres. "A water body SHALL exist logically for the
    /// whole world while only its nearby SEGMENTS are resident", and this is the size of one.
    f32 segment_metres = 256.0F;

    [[nodiscard]] WaterBodyId id() const noexcept { return water_body_id(name); }
};

/// Why a description was refused. A code rather than a string, so a test asserts on the reason
/// rather than on the wording, and so a diagnostic can print both.
enum class BodyProblem : u8 {
    None = 0,
    NoName,
    EmptyBounds,
    /// The mean level is outside the body's own vertical bounds, so the body's still surface is
    /// not inside the volume it claims to occupy.
    LevelOutsideBounds,
    NonPositiveDensity,
    NonPositiveSegment,
    /// `ShallowWater`: declared Planned by the specification's own table.
    BackendPlanned,
    /// `Particle`: declared Deferred, and its seam belongs to `vfx-system`.
    BackendDeferred,
    /// A body of this name is already registered.
    DuplicateName,
};

[[nodiscard]] const char* body_problem_name(BodyProblem problem) noexcept;

/// Check a description on its own, without a registry — so a cooker and an editor can validate one
/// before a world exists. Duplicate names are the registry's to find and are not checked here.
[[nodiscard]] BodyProblem validate_body(const WaterBodyDesc& desc) noexcept;

// --- The registry ---------------------------------------------------------------------------

/// One body, as the registry holds it.
struct WaterBodyRecord {
    WaterBodyDesc desc;
    WaterBodyId id;
    /// Declaration order, which is the tie-break in the resolution order. Stored rather than
    /// implied by array position so that the number survives any future reordering of the array.
    u32 ordinal = 0;
};

/// The bodies of one world, and the declared order that resolves an overlap.
///
/// It holds no simulation state: `WaterSystem` (system.h) owns the ocean model, the river network
/// and the foam field, and takes a registry by reference. The split is the one `environment`'s
/// registry and store draw, for the same reason — a registry that could hold a wave would be a
/// registry every consumer had a reason to reach into.
class WaterRegistry {
public:
    explicit WaterRegistry(Allocator& allocator) noexcept;

    WaterRegistry(const WaterRegistry&) = delete;
    WaterRegistry& operator=(const WaterRegistry&) = delete;

    /// Register a body. Refuses an invalid description, a duplicate name, and a backend the
    /// specification declares Planned or Deferred — the message names the backend, its status and,
    /// for the deferred one, the seam it belongs to.
    [[nodiscard]] Expected<WaterBodyId, Error> add(const WaterBodyDesc& desc) noexcept;

    [[nodiscard]] const WaterBodyRecord* find(WaterBodyId id) const noexcept;
    [[nodiscard]] const WaterBodyDesc* describe(WaterBodyId id) const noexcept;
    [[nodiscard]] Span<const WaterBodyRecord> bodies() const noexcept { return records_.span(); }
    [[nodiscard]] usize size() const noexcept { return records_.size(); }

    /// The body that owns a position, by the declared resolution order: highest `priority` first,
    /// declaration order as the tie-break. A position in no body returns an invalid identity.
    ///
    /// Horizontal containment, not volume containment: a point far above a lake is still over that
    /// lake, and a query above the surface is how a boat's hull finds the water it is floating on.
    [[nodiscard]] WaterBodyId body_at(const world::WorldVec3d& at) const noexcept;

    /// Every body containing a position, in the resolution order. The diagnostic behind "which body
    /// owns it", and what a river mouth inside an ocean's bounds looks like from the outside.
    [[nodiscard]] Status bodies_at(const world::WorldVec3d& at,
                                   Array<WaterBodyId>& out) const noexcept;

    /// Change a body's mean level and, with it, whatever its vertical bounds must become to
    /// contain the new surface. **The only mutation a registered body has**, and it exists because
    /// a flood is a level change: `water` requires level changes to emit navigation dirty regions,
    /// and a body whose level could not change would make that requirement unreachable. Everything
    /// else about a body is fixed at registration, which is why `bodies()` hands out a const span.
    [[nodiscard]] Status set_mean_level(WaterBodyId body, f64 level,
                                        const WaterBounds& bounds) noexcept;

    /// The last refusal, formatted. Valid until the next one; the `Error::message` handed back by
    /// `add()` points into this object's own buffer and has the same lifetime.
    [[nodiscard]] const char* last_refusal() const noexcept { return refusal_; }
    [[nodiscard]] BodyProblem last_problem() const noexcept { return problem_; }

private:
    Allocator* allocator_;
    Array<WaterBodyRecord> records_;
    BodyProblem problem_ = BodyProblem::None;
    /// The formatted refusal. `Error::message` is a `const char*` with no ownership, so a message
    /// naming a backend and its status has to live somewhere with a longer life than the call —
    /// and this is that somewhere, documented rather than leaked.
    char refusal_[256] = {};
};

}  // namespace cy::water

namespace cy {

template <>
struct Hash<water::WaterBodyId> {
    [[nodiscard]] u64 operator()(water::WaterBodyId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

}  // namespace cy
