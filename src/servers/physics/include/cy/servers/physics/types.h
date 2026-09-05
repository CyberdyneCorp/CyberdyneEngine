#pragma once
// The physics vocabulary: motion types, materials, collision filtering, backend tuning, the world
// description, capabilities and per-step statistics. Task 4.2.1.
//
// `physics` — "Engine-owned physics interface": "the rest of the engine SHALL depend only on this
// interface, never on backend types". Everything named here is engine-owned, which is what makes
// that requirement checkable rather than intended: there is no Jolt type to leak because nothing in
// this directory can include one — src/servers/ is layer 2 and Jolt lives at layer 3.
//
// WHY THE TUNING BLOCK IS HERE AND NOT IN THE BACKEND. `physics` requires that "backend-specific
// tuning (solver iterations, penetration slop, baumgarte factor, speculative contact distance,
// sleep thresholds) SHALL be exposed as configuration with documented defaults". Exposed means
// reachable by a project that never links Jolt's headers, so the *names* are engine-owned and the
// backend maps them. A backend that has no analogue for one of them says so through
// `Capabilities`, rather than silently ignoring it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/values/name.h>
#include <cy/servers/physics/handles.h>

namespace cy::physics {

/// The three body kinds `physics` names as components, as one enum on the interface.
///
/// A `Trigger` is NOT a fourth motion type: a trigger is a *collider* property, because `physics`
/// puts several colliders on one body and one of them may be a sensor while the others are solid.
enum class MotionType : u8 {
    /// Never moved by the solver and never by code after creation. The cheapest kind.
    Static = 0,
    /// Moved by code or animation; pushes dynamic bodies and is not pushed by them.
    Kinematic,
    /// Driven by the solver.
    Dynamic,
};

const char* motion_type_name(MotionType value) noexcept;

/// How two materials' friction or restitution combine on a contact.
enum class CombineMode : u8 { Average = 0, Minimum, Multiply, Maximum };

const char* combine_mode_name(CombineMode value) noexcept;

/// Combine two coefficients. `a`'s mode wins when the two disagree and `a` is the more restrictive;
/// the rule is `max(mode)` over the enumeration order, so a Minimum surface stays minimum however
/// it is struck. Stated as a function rather than as a table so both backends compute it the same
/// way and a test can check one against the other.
[[nodiscard]] f32 combine(f32 a, CombineMode a_mode, f32 b, CombineMode b_mode) noexcept;

/// Whether a transform change is a motion or a discontinuity.
///
/// `physics` — "Teleport suppresses interpolation": a teleported body sets a flag that suppresses
/// interpolation for that frame, "avoiding a smear across the world". The distinction lives on the
/// interface rather than in the renderer because only the caller knows which it meant.
enum class TeleportMode : u8 {
    /// A continuous move. Velocity is left alone and the renderer interpolates through it.
    Interpolate = 0,
    /// A discontinuity. Interpolation is suppressed for the frame and the body is woken.
    Teleport,
};

/// `physics` — "Determinism": the policy a backend declares, which a session's determinism profile
/// is validated against. The default is `SamePlatformDeterministic` and cross-platform determinism
/// is NOT guaranteed; see determinism.h, which owns the validation and says why the distinction is
/// load-bearing for networking.
enum class DeterminismPolicy : u8 {
    /// The same binary, architecture and inputs reproduce identical results.
    SamePlatformDeterministic = 0,
    /// Results are authoritative only where an authority produces them and replicates or records
    /// them.
    ExternalAuthority,
    /// Presentation only, excluded from authoritative state.
    NonAuthoritative,
};

const char* determinism_policy_name(DeterminismPolicy value) noexcept;

// --- Collision filtering ---------------------------------------------------------------------
//
// `physics` — "Collision filtering": "each collider has a layer and a mask, and two colliders
// interact only if each one's layer is in the other's mask". The mutual form is the whole of the
// requirement's "One-way filtering is symmetric" scenario, and it is why `accepts()` below is not
// the obvious single-sided test.

/// The number of collision layers. Thirty-two, because a mask is a `u32` and a mask wider than a
/// machine word turns every broad-phase test into a loop.
inline constexpr u32 kCollisionLayerCount = 32;

/// One collider's place in the filter: which layer it is on, and which layers it is willing to
/// touch.
struct CollisionFilter {
    /// `0` to `kCollisionLayerCount - 1`.
    u8 layer = 0;
    /// A bit per layer. The default collides with everything, because a body created without a
    /// filter that silently collided with nothing is a body whose absence is invisible.
    u32 mask = 0xFFFFFFFFU;

    friend constexpr bool operator==(CollisionFilter, CollisionFilter) noexcept = default;
};

/// Mutual acceptance. Both masks must contain the other's layer.
[[nodiscard]] constexpr bool accepts(CollisionFilter a, CollisionFilter b) noexcept {
    const u32 a_bit = 1U << (a.layer & (kCollisionLayerCount - 1U));
    const u32 b_bit = 1U << (b.layer & (kCollisionLayerCount - 1U));
    return (a.mask & b_bit) != 0U && (b.mask & a_bit) != 0U;
}

/// The project-level per-pair collision matrix.
///
/// A layer/mask pair is per collider; this is per *project*, and a pair collides only if both
/// agree. It is stored as one `u32` row per layer and kept symmetric by construction — `allow()`
/// writes both halves — because a matrix that can be asymmetric is a matrix whose two halves
/// disagree in exactly the case nobody tests.
class CollisionMatrix {
public:
    /// Everything collides with everything, which is what a project that never configures a matrix
    /// gets.
    constexpr CollisionMatrix() noexcept {
        for (u32& row : rows_) {
            row = 0xFFFFFFFFU;
        }
    }

    constexpr void allow(u8 a, u8 b, bool enabled) noexcept {
        const u32 ai = a & (kCollisionLayerCount - 1U);
        const u32 bi = b & (kCollisionLayerCount - 1U);
        const u32 a_bit = 1U << ai;
        const u32 b_bit = 1U << bi;
        rows_[ai] = enabled ? (rows_[ai] | b_bit) : (rows_[ai] & ~b_bit);
        rows_[bi] = enabled ? (rows_[bi] | a_bit) : (rows_[bi] & ~a_bit);
    }

    [[nodiscard]] constexpr bool allows(u8 a, u8 b) const noexcept {
        const u32 ai = a & (kCollisionLayerCount - 1U);
        const u32 bi = b & (kCollisionLayerCount - 1U);
        return (rows_[ai] & (1U << bi)) != 0U;
    }

    [[nodiscard]] constexpr u32 row(u8 layer) const noexcept {
        return rows_[layer & (kCollisionLayerCount - 1U)];
    }

private:
    u32 rows_[kCollisionLayerCount] = {};
};

/// The whole filter decision for one pair: the matrix, then both masks. Written once here so that
/// the reference backend, the Jolt backend and every query filter answer it identically.
[[nodiscard]] constexpr bool pair_collides(const CollisionMatrix& matrix, CollisionFilter a,
                                           CollisionFilter b) noexcept {
    return matrix.allows(a.layer, b.layer) && accepts(a, b);
}

// --- Materials -------------------------------------------------------------------------------

/// `physics`' `PhysicsMaterial` component, as the server holds it.
struct MaterialDescription {
    Name name;
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    CombineMode friction_combine = CombineMode::Average;
    CombineMode restitution_combine = CombineMode::Average;
    /// Kilograms per cubic metre. Used when a body derives its mass from its colliders' volumes
    /// rather than stating one — water is 1000, and the default is a light solid.
    f32 density = 1000.0f;
};

// --- Backend tuning --------------------------------------------------------------------------

/// The tuning `physics` requires to be exposed, with the documented defaults it requires.
///
/// The defaults below are Jolt's own, which is deliberate: a project that changes nothing gets the
/// behaviour the backend was tuned for, and a project that changes something gets a number whose
/// meaning is written next to it rather than in another library's header.
struct Tuning {
    /// Solver iterations for velocity and for position. More is stabler and slower.
    u32 velocity_iterations = 10;
    u32 position_iterations = 2;
    /// How deep two bodies may overlap before the solver pushes them apart, in metres. Zero makes
    /// resting contacts jitter, because every frame's small penetration is corrected.
    f32 penetration_slop = 0.02f;
    /// The fraction of remaining penetration resolved per step. 1.0 is an immediate correction and
    /// adds energy.
    f32 baumgarte = 0.2f;
    /// How far ahead of a surface a contact is created, in metres. This is what stops a fast body
    /// from passing through a thin one between two steps.
    f32 speculative_contact_distance = 0.02f;
    /// Below both thresholds for `time_before_sleep_seconds`, a body sleeps.
    f32 sleep_linear_velocity = 0.03f;
    f32 sleep_angular_velocity = 0.05f;
    f32 time_before_sleep_seconds = 0.5f;

    friend constexpr bool operator==(const Tuning&, const Tuning&) noexcept = default;
};

/// Reject a tuning a solver cannot run: a zero iteration count, a negative distance, a slop past
/// the speculative distance. Checked at `create_world`, so a nonsense value fails where it was
/// written rather than as a simulation that quietly misbehaves.
[[nodiscard]] Status validate(const Tuning& tuning) noexcept;

// --- Worlds ----------------------------------------------------------------------------------

struct WorldDescription {
    Name name;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    /// The maximum number of bodies. A hard capacity rather than a hint: a solver sizes its
    /// broad phase from it, and `physics`' determinism guarantee needs allocation not to depend on
    /// the order things happened in.
    u32 body_capacity = 1024;
    /// Broad-phase pairs and contact constraints the solver may hold in one step.
    u32 body_pair_capacity = 4096;
    u32 contact_constraint_capacity = 2048;
    Tuning tuning;
    CollisionMatrix matrix;
};

[[nodiscard]] Status validate(const WorldDescription& description) noexcept;

// --- Capabilities ------------------------------------------------------------------------------
//
// `physics` — "Unsupported feature": "WHEN a backend does not implement soft bodies THEN the
// capability query SHALL report it and creation SHALL fail with a clear diagnostic". Both halves:
// the flags below are the query, and the creation call returns `ErrorCode::Unsupported` naming the
// backend. A backend that reports a capability it does not have fails its own conformance suite.

struct Capabilities {
    /// Contacts between solid bodies are resolved, not merely detected.
    bool contact_resolution = false;
    /// Constraints of the kinds in constraints.h.
    bool constraints = false;
    bool triangle_meshes = false;
    bool convex_hulls = false;
    bool height_fields = false;
    bool soft_bodies = false;
    bool vehicles = false;
    bool buoyancy = false;
    /// Continuous collision detection for fast bodies.
    bool continuous_collision = false;
    /// The internal parallelism runs on engine job workers rather than on threads the backend
    /// started. `physics` — "One job system".
    bool uses_engine_jobs = false;
    /// The strongest determinism this backend offers.
    DeterminismPolicy determinism = DeterminismPolicy::NonAuthoritative;
};

// --- Statistics ---------------------------------------------------------------------------------
//
// `physics` — "Diagnosing a slow step": the breakdown "SHALL show whether cost is in broad phase,
// narrow phase, or solving". Three timers rather than one, and the counts beside them, because a
// step that is slow because it has 40 000 contacts and a step that is slow because the solver is
// iterating are the same number and different bugs.

struct StepStatistics {
    u32 body_count = 0;
    u32 active_body_count = 0;
    u32 contact_count = 0;
    u32 constraint_count = 0;
    u32 island_count = 0;
    Nanoseconds broad_phase_ns = 0;
    Nanoseconds narrow_phase_ns = 0;
    Nanoseconds solve_ns = 0;
    Nanoseconds total_ns = 0;
    /// The simulation tick this step advanced to. Never a wall clock — see stepper.h.
    u64 tick = 0;
};

/// What one shape cache has done, which is how `physics`' "Shape sharing" scenario is asserted:
/// 1 000 entities with an identical box collider create one shape.
struct ShapeStatistics {
    /// Distinct shapes the backend allocated.
    u32 unique_shapes = 0;
    /// `create_shape` calls. `unique_shapes` below this is the cache working.
    u32 requests = 0;
    u32 cache_hits = 0;
};

}  // namespace cy::physics
