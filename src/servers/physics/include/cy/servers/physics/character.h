#pragma once
// The capsule character controller: collide-and-slide over the server's queries. Task 4.2.5.
//
// `physics` — "Character controller": capsule-based collide-and-slide with "ground detection and a
// maximum slope angle, step offset for stairs, ceiling detection, a skin width, a maximum iteration
// count, pushing of dynamic bodies with configurable force, and moving platform support that
// inherits platform motion", in two modes — `Grounded` and `Floating`.
//
// ================================================================================================
// WHY THIS IS ENGINE CODE AND NOT A BACKEND CALL
// ================================================================================================
//
// Jolt has a character controller. So does every other solver, and every one of them has different
// stair behaviour, a different definition of "grounded" at exactly the maximum slope, and a
// different answer for a capsule wedged into an inside corner. `physics` requires that "WHEN a
// project selects a different physics backend THEN no gameplay code, component definition, or scene
// asset SHALL require changes" — and a character that walks differently after a backend swap has
// changed gameplay whether or not any code moved.
//
// So the controller is written ONCE, here, against `PhysicsServer`'s shape cast and overlap. It
// works identically over the reference backend and over Jolt, which is what makes its suite a
// conformance test for both of them rather than a test of one. The cost is that it does not use
// Jolt's `CharacterVirtual`; the benefit is that "walking up stairs" means one thing in this
// engine.
//
// ================================================================================================
// THE ALGORITHM, IN THE ORDER IT RUNS
// ================================================================================================
//
//   1. depenetrate — an overlap test, pushed out along the deepest normal, `kDepenetrationPasses`
//      times at most. A controller that starts a step already inside geometry and slides anyway is
//      the one that ends up outside the level.
//   2. vertical — gravity is integrated into `velocity` (Grounded only), then the down move is
//      swept. What it hits sets the ground state.
//   3. horizontal — the desired horizontal motion is swept and slid, up to `max_iterations` times.
//      A surface steeper than `max_slope` is a WALL: the motion is projected onto it and the
//      character does not climb.
//   4. step up / step down — if the horizontal move was blocked by something within `step_offset`
//      of the feet, it is retried from `step_offset` higher and then dropped back onto the ground.
//      `physics` — "Walking up stairs": lifted onto the step "without a jump".
//   5. platform — if the ground body moved since the last step, its motion is added. On leaving,
//      `inherit_platform_velocity` decides whether its velocity is kept.
//
// SKIN WIDTH is subtracted from every sweep, so the capsule stops that far short of a surface and
// the next step's depenetration has something to work with. A controller that sweeps to exactly
// zero distance is a controller that is in contact with everything it touches, and floating-point
// says whether "in contact" means "inside".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/servers/physics/server.h>

namespace cy::physics {

/// `physics`' two modes.
enum class CharacterMode : u8 {
    /// Gravity, slopes, stairs, a ground plane.
    Grounded = 0,
    /// Six degrees of freedom, for swimming and flying. No gravity, no ground, no stepping.
    Floating,
};

/// What the controller decided it is standing on.
enum class GroundState : u8 {
    /// On a surface no steeper than `max_slope_radians`.
    Grounded = 0,
    /// Touching a surface steeper than the maximum. `physics` — "Steep slope": the character "SHALL
    /// be reported as touching a wall rather than grounded, and SHALL slide down".
    OnSteepSlope,
    InAir,
};

const char* ground_state_name(GroundState value) noexcept;

/// What happens to a platform's velocity when the character steps off it.
enum class InheritedVelocity : u8 {
    /// Keep it: stepping off a moving train launches you along the train.
    Inherit = 0,
    /// Drop it: stepping off a lift does not fling you.
    Discard,
    /// Keep the horizontal part only, which is what most games actually want.
    HorizontalOnly,
};

struct CharacterDescription {
    /// The capsule. `height` is the TOTAL height including both caps, because that is the number an
    /// artist has; the shape's `half_height` is derived as `height / 2 - radius`.
    f32 radius = 0.3f;
    f32 height = 1.8f;

    /// Above this a surface is a wall. 45 degrees.
    f32 max_slope_radians = 0.7853982f;
    /// The tallest step that is climbed rather than blocked.
    f32 step_offset = 0.35f;
    /// How far short of a surface the capsule stops.
    f32 skin_width = 0.02f;
    /// Slide iterations per horizontal move.
    u32 max_iterations = 4;

    CharacterMode mode = CharacterMode::Grounded;
    f32 gravity_scale = 1.0f;
    /// Kilograms. What a pushed dynamic body is pushed with.
    f32 mass = 70.0f;

    /// `physics`: "pushing of dynamic bodies with configurable force".
    bool push_dynamic_bodies = true;
    /// Newtons. Zero pushes nothing however `push_dynamic_bodies` is set.
    f32 push_force = 300.0f;

    InheritedVelocity inherit_platform_velocity = InheritedVelocity::HorizontalOnly;

    /// The layer and mask the sweeps use. The character's own body, if it has one, is always
    /// ignored regardless of this.
    CollisionFilter filter;

    /// Up, in world space. `+Y`, and configurable because a wall-walking game exists.
    Vec3 up{0.0f, 1.0f, 0.0f};

    Transform start;
    UserData user_data = 0;
};

[[nodiscard]] Status validate(const CharacterDescription& description) noexcept;

/// What one `move()` produced. Every field is an output; nothing here is written by the caller.
struct CharacterState {
    /// Position of the capsule's centre, and the character's facing.
    Transform transform;
    /// The controller's own velocity, after gravity, after collisions and after the platform.
    Vec3 velocity{0.0f, 0.0f, 0.0f};

    GroundState ground = GroundState::InAir;
    Vec3 ground_normal{0.0f, 1.0f, 0.0f};
    BodyHandle ground_body;
    MaterialHandle ground_material;

    bool touching_ceiling = false;
    bool touching_wall = false;
    /// True on the step the character was lifted onto a stair. What an animation graph reads to
    /// pick a step-up animation, and what the stairs test asserts.
    bool stepped_up = false;

    /// The platform's contribution to this step's motion, per second. Zero off a platform.
    Vec3 platform_velocity{0.0f, 0.0f, 0.0f};

    /// Sweeps this move performed. A budget number, and the way a pathological corner shows up as
    /// something other than a frame-time spike.
    u32 sweeps = 0;
};

/// The desired motion for one step.
struct CharacterInput {
    /// Metres per second, in world space. In `Grounded` mode the component along `up` is taken as a
    /// deliberate vertical intent (a jump) and everything else is horizontal.
    Vec3 desired_velocity{0.0f, 0.0f, 0.0f};
    /// Replaces the vertical velocity this step. `physics`' jump, kept separate from
    /// `desired_velocity` so a controller that is falling does not have its fall overwritten by a
    /// zero.
    bool jump = false;
    f32 jump_speed = 0.0f;
};

/// The controller. Owns a shape and, optionally, a kinematic body so other bodies can see it.
///
/// It does NOT own the world: `PhysicsServer&` and `WorldHandle` outlive it, and destroying it
/// releases its shape and its body.
class CharacterController {
public:
    CharacterController(PhysicsServer& server, WorldHandle world) noexcept;
    ~CharacterController();

    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    /// Create the capsule and, when `with_body`, a kinematic body carrying it so that dynamic
    /// bodies collide with the character rather than passing through it.
    [[nodiscard]] Status create(const CharacterDescription& description,
                                bool with_body = true) noexcept;

    /// One fixed step. `delta` is the simulation step — see stepper.h; a controller stepped with a
    /// frame time is a controller whose stair behaviour depends on the frame rate.
    [[nodiscard]] Status move(f32 delta, const CharacterInput& input) noexcept;

    [[nodiscard]] const CharacterState& state() const noexcept { return state_; }
    [[nodiscard]] const CharacterDescription& description() const noexcept { return description_; }
    [[nodiscard]] BodyHandle body() const noexcept { return body_; }
    [[nodiscard]] ShapeHandle shape() const noexcept { return shape_; }

    /// Move without sweeping. Suppresses interpolation for the frame, like any teleport.
    [[nodiscard]] Status teleport(const Transform& transform) noexcept;

    /// Replace the velocity outright — a launch pad, a cutscene, a reconciliation.
    void set_velocity(Vec3 velocity) noexcept { state_.velocity = velocity; }

private:
    struct SweepResult {
        bool hit = false;
        /// How far the capsule may travel: the raw distance less the skin width, clamped at zero.
        f32 distance = 0.0f;
        /// The distance the query reported, before the skin width was taken off and before the
        /// clamp. Negative separation is not representable — that is what `started_penetrating` is
        /// for — but a value BELOW the skin width is, and it is what `update_ground` needs to snap
        /// the capsule back to exactly one skin width above the floor. See update_ground().
        f32 raw_distance = 0.0f;
        Vec3 normal{0.0f, 1.0f, 0.0f};
        BodyHandle body;
        MaterialHandle material;
        bool started_penetrating = false;
    };

    [[nodiscard]] QueryFilter query_filter() const noexcept;
    [[nodiscard]] SweepResult sweep(Vec3 from, Vec3 direction, f32 distance) noexcept;
    /// Push out of anything the capsule already overlaps. Returns the correction applied.
    Vec3 depenetrate(Vec3 position) noexcept;
    /// Steps 3 and 4: slide the horizontal motion, retrying over a step when it is blocked low.
    Vec3 slide(Vec3 position, Vec3 motion, bool allow_step_up) noexcept;
    void update_ground(Vec3& position, f32 delta) noexcept;
    void push_bodies(const SweepResult& result, Vec3 direction, f32 delta) noexcept;
    /// True when a surface with this normal is standable.
    [[nodiscard]] bool is_walkable(Vec3 normal) const noexcept;

    PhysicsServer* server_;
    WorldHandle world_;
    CharacterDescription description_;
    CharacterState state_;
    ShapeHandle shape_;
    BodyHandle body_;
    /// The ground body's transform at the end of the previous step, for the platform delta.
    Transform previous_ground_transform_;
    bool had_ground_ = false;
    f32 cos_max_slope_ = 0.0f;
};

}  // namespace cy::physics
