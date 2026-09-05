#pragma once
// Fixed-step integration on the simulation clock, and the interpolation the renderer reads.
// Task 4.2.3.
//
// `physics` — "Fixed-step integration": physics "SHALL step exactly once per simulation tick at the
// fixed timestep, within the `Physics` stage, and SHALL never step during a variable-rate frame.
// Transforms written by physics SHALL be published to `LocalTransform`/`WorldTransform` after the
// step, and rendering SHALL interpolate between the previous and current physics transforms using
// the frame's interpolation alpha."
//
// ================================================================================================
// THE CLOCK IS THE ARGUMENT, WHICH IS WHAT MAKES THE REQUIREMENT STRUCTURAL
// ================================================================================================
//
// `determinism::SimulationClock` "cannot read a clock" — its own header says so, and it has no
// member that calls one. A stepper built on it therefore cannot step on wall time even by mistake:
// `tick()` takes the step from `clock.delta_seconds()` and the tick number from `clock.tick()`, and
// there is no other source of either reachable from layer 2. The other half — "never during a
// variable-rate frame" — is that `tick()` is the only method that calls `PhysicsServer::step()`,
// and `interpolate()`, which is what a frame calls, is `const`.
//
// ================================================================================================
// WHY THE PREVIOUS TRANSFORM IS STORED HERE AND NOT IN THE SERVER
// ================================================================================================
//
// Interpolation is presentation. `simulation-and-determinism` classifies it as such and a solver
// that kept a previous transform would be carrying presentation state inside authoritative state,
// where it would be hashed, checkpointed and replicated. So the previous transform lives in the
// stepper, beside the tick that produced it, and `PhysicsServer::hash_state()` never sees it.
//
// ================================================================================================
// WHAT THIS DOES NOT DO, AND WHO DOES IT
// ================================================================================================
//
// It does not write `cy::scene::LocalTransform`. That component is layer 4 and this is layer 2, so
// the publication is inverted: the stepper hands each moved body's transform to a `TransformSink`
// the caller implements, and the caller at layer 4 is the one that writes the component. Anything
// else would be a server reaching into ECS storage, which `engine-architecture` calls a layering
// defect in as many words.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/physics/server.h>

namespace cy::physics {

/// Where a stepped body's transform goes. Implemented above layer 2 — see the header comment.
class TransformSink {
public:
    TransformSink() = default;
    virtual ~TransformSink() = default;

    TransformSink(const TransformSink&) = delete;
    TransformSink& operator=(const TransformSink&) = delete;
    TransformSink(TransformSink&&) = delete;
    TransformSink& operator=(TransformSink&&) = delete;

    /// Called once per moved body, after the step. `teleported` is `physics`' "Teleport suppresses
    /// interpolation" flag: the frame that receives it must not interpolate this body.
    virtual void publish(BodyHandle body, UserData user_data, const Transform& transform,
                         bool teleported) noexcept = 0;
};

/// One tracked body's presentation state.
struct InterpolationRecord {
    Transform previous;
    Transform current;
    /// The tick `current` was produced on. A body created between two ticks has `previous ==
    /// current` and interpolates to a standstill rather than from the origin.
    u64 tick = 0;
    bool teleported = false;
};

/// Drives one physics world from the simulation clock and keeps the interpolation pair.
///
/// Not thread-safe, and not meant to be: it is used at the quiesced commit boundary, in the
/// `Physics` stage, on the simulation thread.
class PhysicsStepper {
public:
    PhysicsStepper(PhysicsServer& server, WorldHandle world, Allocator& allocator) noexcept;

    PhysicsStepper(const PhysicsStepper&) = delete;
    PhysicsStepper& operator=(const PhysicsStepper&) = delete;

    /// Start tracking a body's transform for interpolation. Idempotent.
    [[nodiscard]] Status track(BodyHandle body) noexcept;
    void untrack(BodyHandle body) noexcept;
    [[nodiscard]] u32 tracked_count() const noexcept { return static_cast<u32>(records_.size()); }

    /// ONE fixed step, at the clock's rate, for the clock's current tick.
    ///
    /// `sink` may be null, which is what a headless determinism test passes. The clock is `const`:
    /// advancing it is the runtime's, and a stepper that advanced it could step twice for one tick.
    [[nodiscard]] Status tick(const determinism::SimulationClock& clock,
                              TransformSink* sink) noexcept;

    /// The presentation transform for a frame at `alpha`, which is
    /// `SimulationClock::interpolation_alpha()`.
    ///
    /// `physics` — "Rendering interpolates physics": above the physics rate, rendered positions
    /// "SHALL interpolate between physics ticks rather than stepping". A teleported body returns
    /// its current transform unchanged, which is the "Teleport suppresses interpolation" scenario.
    [[nodiscard]] Expected<Transform, Error> interpolate(BodyHandle body, f32 alpha) const noexcept;

    [[nodiscard]] const InterpolationRecord* record(BodyHandle body) const noexcept;

    /// Ticks this stepper has run. A test asserts it against the clock's, which is how "exactly
    /// once per simulation tick" is checked rather than assumed.
    [[nodiscard]] u64 steps() const noexcept { return steps_; }
    [[nodiscard]] u64 last_tick() const noexcept { return last_tick_; }

private:
    PhysicsServer* server_;
    WorldHandle world_;
    /// body bits -> index into `records_`. A map rather than a handle-indexed array because a
    /// stepper tracks the few hundred bodies that have a visual, not every body in the world.
    HashMap<u64, u32> index_;
    Array<BodyHandle> bodies_;
    Array<InterpolationRecord> records_;
    u64 steps_ = 0;
    u64 last_tick_ = 0;
};

}  // namespace cy::physics
