#pragma once
// `PhysicsServer` — the engine-owned physics interface every backend implements. Task 4.2.1.
//
// ================================================================================================
// THE ORDER THIS WAS WRITTEN IN IS THE DESIGN
// ================================================================================================
//
// design.md §4, and `thirdparty-dependencies`: "PhysicsServer and the audio driver layer are
// defined and exercised by a trivial implementation BEFORE Jolt and miniaudio are linked. M0
// established the pattern with DisplayServer before SDL3, and M3 with the RHI before Vulkan — the
// null backend written first is why the RHI is an interface rather than a wrapper."
//
// This header and `cy::physics::reference::ReferenceServer` were written and made green before a
// line of Jolt existed. The retained reference backend is not ceremony: it is what proves at every
// build that the interface does not leak the library, and it is what lets a determinism test run
// with `-D CY_PHYSICS=OFF`.
//
// ================================================================================================
// WHAT THIS SERVER IS NOT ALLOWED TO SEE
// ================================================================================================
//
// `src/servers/` is layer 2. `src/backends/` is layer 3, `src/scene/` and `src/rendering/` are 4,
// `src/runtime/` is 5. So this file may not include a Jolt header, a component, a node, a world, or
// the runtime's server registry — the layer checker fails the build over any of them. That is what
// makes `physics`' "Backend types do not leak" a property of the build rather than a review note:
//
//   * "WHEN a project selects a different physics backend THEN no gameplay code, component
//     definition, or scene asset SHALL require changes" is true because gameplay reaches physics
//     through this header and there is nothing else to reach.
//   * a body carries a `UserData`, never an `ecs::Entity`. The server literally cannot dereference
//     one; see handles.h.
//   * the fixed-step integration (stepper.h) drives this interface from M2's simulation clock,
//     which is layer 0. There is no wall clock reachable from here at all.
//
// THE FOUR METHODS AT THE TOP are deliberately `cy::runtime::Server`'s four, with the same
// signatures, so that the layer-5 adapter which registers physics in the `ServerRegistry` is four
// forwarding lines and no decisions. `RenderServer` does the same thing for the same reason.
//
// ================================================================================================
// THREAD SAFETY, STATED ONCE
// ================================================================================================
//
// Mutating calls — create, destroy, set, `step` — are called from the simulation thread at the
// quiesced commit boundary and are NOT thread-safe.
//
// Query calls are `const` and ARE thread-safe: `physics` requires that "many entities raycast
// concurrently from a parallel system" is safe and "SHALL NOT mutate simulation state". `const` is
// the half the compiler checks; the other half is that a backend must not memoise into a mutable
// member on the query path, which its own conformance suite asserts by running the parallel case.
//
// A query DURING a step is a different thing and is rejected: `physics` — "WHEN a query is
// attempted while the physics step is in progress THEN it SHALL be rejected in development builds
// with a diagnostic, since the world is mid-solve". `stepping()` below is that flag, and every
// query checks it — in development builds only, because in Shipping the check is a branch on the
// hot path and the situation it detects is a programmer error that development already caught.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/hash.h>
#include <cy/servers/physics/body.h>
#include <cy/servers/physics/constraints.h>
#include <cy/servers/physics/debug.h>
#include <cy/servers/physics/events.h>
#include <cy/servers/physics/handles.h>
#include <cy/servers/physics/queries.h>
#include <cy/servers/physics/shapes.h>
#include <cy/servers/physics/types.h>

namespace cy::physics {

/// What one `step()` is told. Everything the solver needs that is not world state.
///
/// THE DELTA IS AN ARGUMENT AND NOT A CLOCK READING, and that is the whole of `physics`'
/// "Fixed-step integration": physics "SHALL step exactly once per simulation tick at the fixed
/// timestep" and "SHALL never step during a variable-rate frame". A server that read a clock could
/// be stepped from a frame; a server that is handed a delta by `PhysicsStepper` (stepper.h) cannot
/// be, because the stepper is the only thing that computes one and it computes it from
/// `SimulationClock`.
struct StepInput {
    /// Seconds. `SimulationClock::delta_seconds()`, unchanged — never a measured frame time.
    f32 delta_seconds = 1.0f / 60.0f;
    /// The tick this step advances to. Recorded in the statistics and mixed into the state hash, so
    /// a divergence report can say *when*.
    u64 tick = 0;
    /// Sub-steps within the fixed step. More is stabler for fast bodies; the delta is divided.
    u32 collision_steps = 1;
};

class PhysicsServer {
public:
    PhysicsServer() = default;
    virtual ~PhysicsServer() = default;

    PhysicsServer(const PhysicsServer&) = delete;
    PhysicsServer& operator=(const PhysicsServer&) = delete;
    PhysicsServer(PhysicsServer&&) = delete;
    PhysicsServer& operator=(PhysicsServer&&) = delete;

    // --- `cy::runtime::Server`'s four, verbatim ------------------------------------------------

    /// "jolt", "reference", "null". Named in the diagnostic when the backend that ran was not the
    /// one that was asked for.
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
    [[nodiscard]] virtual Status initialize() noexcept = 0;
    virtual void shutdown() noexcept = 0;
    [[nodiscard]] virtual bool is_null_backend() const noexcept { return false; }

    // --- What this backend can do -----------------------------------------------------------

    [[nodiscard]] virtual Capabilities capabilities() const noexcept = 0;

    /// True between the start and the end of `step()`. Queries are rejected while it is set.
    [[nodiscard]] virtual bool stepping() const noexcept = 0;

    // --- Worlds --------------------------------------------------------------------------------

    [[nodiscard]] virtual Expected<WorldHandle, Error> create_world(
        const WorldDescription& description) noexcept = 0;
    [[nodiscard]] virtual Status destroy_world(WorldHandle world) noexcept = 0;
    [[nodiscard]] virtual Status set_gravity(WorldHandle world, Vec3 gravity) noexcept = 0;

    /// Advance one fixed step. The only call that mutates simulation state.
    [[nodiscard]] virtual Status step(WorldHandle world, const StepInput& input) noexcept = 0;

    [[nodiscard]] virtual Expected<StepStatistics, Error> statistics(
        WorldHandle world) const noexcept = 0;

    /// The events the last `step()` produced, valid until the next one.
    [[nodiscard]] virtual Expected<Span<const ContactEvent>, Error> events(
        WorldHandle world) const noexcept = 0;

    /// The constraints the last `step()` broke, valid until the next one.
    [[nodiscard]] virtual Expected<Span<const ConstraintBroken>, Error> broken_constraints(
        WorldHandle world) const noexcept = 0;

    // --- Materials and shapes -------------------------------------------------------------------

    [[nodiscard]] virtual Expected<MaterialHandle, Error> create_material(
        const MaterialDescription& description) noexcept = 0;
    [[nodiscard]] virtual Status destroy_material(MaterialHandle material) noexcept = 0;
    [[nodiscard]] virtual Expected<MaterialDescription, Error> material(
        MaterialHandle material) const noexcept = 0;

    /// Shapes are shared. Two identical descriptions return the SAME handle and the second call is
    /// a cache hit — `physics`' "Shape sharing" scenario, which `shape_statistics()` is how a test
    /// asserts rather than infers.
    [[nodiscard]] virtual Expected<ShapeHandle, Error> create_shape(
        const ShapeDescription& description) noexcept = 0;
    [[nodiscard]] virtual Status destroy_shape(ShapeHandle shape) noexcept = 0;
    [[nodiscard]] virtual Expected<ShapeStatistics, Error> shape_statistics() const noexcept = 0;

    /// `physics` — "A crater updates collision locally": "WHEN terrain is deformed by an explosion
    /// THEN only the affected region's collision SHALL be updated". `samples` is a
    /// `width * depth` block in row-major order, replacing the corresponding block of the shape.
    [[nodiscard]] virtual Status update_height_field(ShapeHandle shape, u32 x, u32 z, u32 width,
                                                     u32 depth,
                                                     Span<const f32> samples) noexcept = 0;

    // --- Bodies --------------------------------------------------------------------------------

    [[nodiscard]] virtual Expected<BodyHandle, Error> create_body(
        WorldHandle world, const BodyDescription& description) noexcept = 0;

    /// `physics` — "A region's collision arrives at once": a cell's collision "SHALL be registered
    /// in bulk rather than one shape at a time". One call, one broad-phase rebuild.
    [[nodiscard]] virtual Status create_bodies(WorldHandle world,
                                               Span<const BodyDescription> descriptions,
                                               Span<BodyHandle> out) noexcept = 0;

    [[nodiscard]] virtual Status destroy_body(BodyHandle body) noexcept = 0;
    [[nodiscard]] virtual Status destroy_bodies(Span<const BodyHandle> bodies) noexcept = 0;

    [[nodiscard]] virtual Expected<BodyState, Error> body_state(BodyHandle body) const noexcept = 0;
    [[nodiscard]] virtual Expected<MassProperties, Error> mass_properties(
        BodyHandle body) const noexcept = 0;
    [[nodiscard]] virtual Expected<UserData, Error> body_user_data(
        BodyHandle body) const noexcept = 0;
    [[nodiscard]] virtual bool body_alive(BodyHandle body) const noexcept = 0;

    [[nodiscard]] virtual Status set_body_transform(BodyHandle body, const Transform& transform,
                                                    TeleportMode mode) noexcept = 0;
    [[nodiscard]] virtual Status set_body_velocity(BodyHandle body, Vec3 linear,
                                                   Vec3 angular) noexcept = 0;
    [[nodiscard]] virtual Status set_body_motion_type(BodyHandle body,
                                                      MotionType motion) noexcept = 0;
    [[nodiscard]] virtual Status set_body_filter(BodyHandle body,
                                                 CollisionFilter filter) noexcept = 0;
    [[nodiscard]] virtual Status set_body_gravity_scale(BodyHandle body, f32 scale) noexcept = 0;

    /// Wake or sleep. Waking a static body is an error rather than a no-op: it means the caller
    /// believes it is dynamic.
    [[nodiscard]] virtual Status set_body_awake(BodyHandle body, bool awake) noexcept = 0;

    /// `physics` — "per-body ignore lists SHALL be supported for exceptions". Symmetric: ignoring
    /// b from a also ignores a from b, because the filter rule is mutual and an asymmetric ignore
    /// list would be the one place it is not.
    [[nodiscard]] virtual Status set_pair_ignored(BodyHandle a, BodyHandle b,
                                                  bool ignored) noexcept = 0;

    [[nodiscard]] virtual Status add_force(BodyHandle body, Vec3 force) noexcept = 0;
    [[nodiscard]] virtual Status add_torque(BodyHandle body, Vec3 torque) noexcept = 0;
    [[nodiscard]] virtual Status add_impulse(BodyHandle body, Vec3 impulse) noexcept = 0;
    [[nodiscard]] virtual Status add_impulse_at(BodyHandle body, Vec3 impulse,
                                                Vec3 world_point) noexcept = 0;
    [[nodiscard]] virtual Status add_angular_impulse(BodyHandle body, Vec3 impulse) noexcept = 0;

    // --- Constraints ---------------------------------------------------------------------------

    [[nodiscard]] virtual Expected<ConstraintHandle, Error> create_constraint(
        WorldHandle world, const ConstraintDescription& description) noexcept = 0;
    [[nodiscard]] virtual Status destroy_constraint(ConstraintHandle constraint) noexcept = 0;
    [[nodiscard]] virtual Status set_constraint_enabled(ConstraintHandle constraint,
                                                        bool enabled) noexcept = 0;
    [[nodiscard]] virtual Status set_constraint_motor(ConstraintHandle constraint,
                                                      const MotorSettings& motor) noexcept = 0;

    // --- Queries. `const`, thread-safe, rejected mid-step
    // -----------------------------------------

    /// The first hit, nearest first. `has_value()` with a null `body` means "nothing was hit",
    /// which is not an error — an error is a bad world handle or a query during the step.
    [[nodiscard]] virtual Expected<RayCastHit, Error> raycast(
        WorldHandle world, const RayCastInput& input, const QueryFilter& filter) const noexcept = 0;

    /// Every hit, up to `out.size()`. Returns how many were written; hits are sorted by distance,
    /// because an unsorted "all hits" makes every caller sort and two of them sort differently.
    [[nodiscard]] virtual Expected<u32, Error> raycast_all(WorldHandle world,
                                                           const RayCastInput& input,
                                                           const QueryFilter& filter,
                                                           Span<RayCastHit> out) const noexcept = 0;

    [[nodiscard]] virtual Expected<ShapeCastHit, Error> shape_cast(
        WorldHandle world, const ShapeCastInput& input,
        const QueryFilter& filter) const noexcept = 0;

    [[nodiscard]] virtual Expected<u32, Error> overlap(WorldHandle world, const OverlapInput& input,
                                                       const QueryFilter& filter,
                                                       Span<OverlapHit> out) const noexcept = 0;

    [[nodiscard]] virtual Expected<u32, Error> overlap_point(
        WorldHandle world, Vec3 point, const QueryFilter& filter,
        Span<OverlapHit> out) const noexcept = 0;

    [[nodiscard]] virtual Expected<ClosestPoint, Error> closest_point(
        WorldHandle world, const ClosestPointInput& input,
        const QueryFilter& filter) const noexcept = 0;

    // --- Determinism and debugging
    // ----------------------------------------------------------------

    [[nodiscard]] virtual DeterminismPolicy determinism_policy() const noexcept = 0;

    /// Fold the world's authoritative state into `tree`: one `HashLevel::Entity` node per body,
    /// whose id is the body handle's bits, walked in the world's own **creation order**.
    ///
    /// WHAT THE ORDER HAS TO BE IS STABLE, NOT SORTED. `StateHashTree::compare` matches children by
    /// `(level, id)` rather than by position, so the order does not affect which body a divergence
    /// is reported against — but the ROOT hash folds children in order, so two runs must walk the
    /// same one. Creation order is that order and is a property of the engine's own array; a
    /// backend's internal body order is not, and sorting would be a second order to keep in step
    /// with the step's own walk. Task 4.2.6; determinism.h owns the comparison and the report.
    [[nodiscard]] virtual Status hash_state(WorldHandle world,
                                            determinism::StateHashTree& tree) const noexcept = 0;

    [[nodiscard]] virtual Status debug_draw(WorldHandle world, DebugDrawFlags flags,
                                            DebugDrawSink& sink) const noexcept = 0;
};

}  // namespace cy::physics
