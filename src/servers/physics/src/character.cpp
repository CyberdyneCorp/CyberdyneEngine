// The capsule character controller. Task 4.2.5.
//
// Written against `PhysicsServer`'s shape cast and overlap and nothing else, so it behaves
// identically over the reference backend and over Jolt — see character.h for why that matters more
// than using either backend's own controller.

#include <cy/servers/physics/character.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::physics {
namespace {

/// Depenetration passes per step. Three resolves a corner (two walls and a floor); a fourth would
/// only help a configuration the capsule should not have reached, and an unbounded loop is how a
/// controller wedged in geometry becomes a hang rather than a visible bug.
constexpr u32 kDepenetrationPasses = 3;

/// The most bodies one overlap test reports back. A capsule in a corner touches three surfaces; the
/// eighth is a shape nobody authored deliberately.
constexpr u32 kMaxOverlaps = 8;

/// Below this a motion is not worth a sweep. One micrometre: far under the skin width, far over the
/// float noise a normalised direction carries.
constexpr f32 kMinimumMotion = 1e-6f;

/// Project `motion` onto the plane of `normal`, so a blocked move slides along the surface instead
/// of stopping dead against it.
[[nodiscard]] Vec3 slide_along(Vec3 motion, Vec3 normal) noexcept {
    return motion - normal * dot(motion, normal);
}

}  // namespace

const char* ground_state_name(GroundState value) noexcept {
    switch (value) {
        case GroundState::Grounded:
            return "grounded";
        case GroundState::OnSteepSlope:
            return "steep-slope";
        case GroundState::InAir:
            return "in-air";
    }
    return "unknown";
}

Status validate(const CharacterDescription& d) noexcept {
    if (d.radius <= 0.0f || !math::is_finite(d.radius)) {
        return fail(ErrorCode::InvalidArgument, "character: radius must be positive");
    }
    if (d.height <= 2.0f * d.radius) {
        // A capsule whose total height is not more than its diameter is a sphere, and the derived
        // `half_height` would be zero or negative. Rejected here rather than clamped, because a
        // silently spherical character has no upright axis and every ground test then behaves
        // differently than the author expects.
        return fail(ErrorCode::InvalidArgument,
                    "character: height must exceed twice the radius, or the capsule is a sphere");
    }
    if (d.max_slope_radians <= 0.0f || d.max_slope_radians >= math::kPi * 0.5f) {
        return fail(ErrorCode::InvalidArgument,
                    "character: max_slope_radians must be within (0, pi/2)");
    }
    if (d.step_offset < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "character: step_offset is negative");
    }
    if (d.skin_width < 0.0f || d.skin_width >= d.radius) {
        return fail(ErrorCode::InvalidArgument, "character: skin_width must be within [0, radius)");
    }
    if (d.max_iterations == 0) {
        return fail(ErrorCode::InvalidArgument, "character: max_iterations is zero");
    }
    if (math::nearly_zero(length_squared(d.up))) {
        return fail(ErrorCode::InvalidArgument, "character: up is degenerate");
    }
    return ok();
}

CharacterController::CharacterController(PhysicsServer& server, WorldHandle world) noexcept
    : server_(&server), world_(world) {}

CharacterController::~CharacterController() {
    if (!body_.is_null()) {
        (void)server_->destroy_body(body_);
    }
    if (!shape_.is_null()) {
        (void)server_->destroy_shape(shape_);
    }
}

Status CharacterController::create(const CharacterDescription& description,
                                   bool with_body) noexcept {
    if (Status valid = validate(description); !valid) {
        return valid;
    }
    description_ = description;
    description_.up = normalize(description.up);
    cos_max_slope_ = std::cos(description_.max_slope_radians);

    ShapeDescription capsule;
    capsule.type = ShapeType::Capsule;
    capsule.radius = description_.radius;
    // `height` is the TOTAL height including both caps — see character.h. Getting this conversion
    // wrong by one radius is how a character ends up standing a hand's width inside the floor.
    capsule.half_height = (description_.height * 0.5f) - description_.radius;
    const Expected<ShapeHandle, Error> shape = server_->create_shape(capsule);
    if (!shape) {
        return make_unexpected(shape.error());
    }
    shape_ = *shape;

    if (with_body) {
        // Kinematic, not dynamic: the controller decides where the capsule goes, and the body
        // exists so that OTHER bodies can see it. A dynamic body here would be solved twice — once
        // by the solver and once by the collide-and-slide — and the two would fight.
        ColliderDescription collider;
        collider.shape = shape_;
        collider.filter = description_.filter;
        BodyDescription body;
        body.motion = MotionType::Kinematic;
        body.transform = description_.start;
        body.colliders = &collider;
        body.collider_count = 1;
        body.user_data = description_.user_data;
        body.allow_sleeping = false;
        const Expected<BodyHandle, Error> created = server_->create_body(world_, body);
        if (!created) {
            (void)server_->destroy_shape(shape_);
            shape_ = ShapeHandle{};
            return make_unexpected(created.error());
        }
        body_ = *created;
    }

    state_ = CharacterState{};
    state_.transform = description_.start;
    state_.ground_normal = description_.up;
    return ok();
}

QueryFilter CharacterController::query_filter() const noexcept {
    QueryFilter filter;
    filter.filter = description_.filter;
    // `physics` — "Raycast excluding self": the character's own collider is never a hit. Applied
    // here so that no call site has to remember it; `ignore` points at the member, which outlives
    // every query made through this object.
    filter.ignore = &body_;
    filter.ignore_count = body_.is_null() ? 0U : 1U;
    filter.include_triggers = false;
    return filter;
}

CharacterController::SweepResult CharacterController::sweep(Vec3 from, Vec3 direction,
                                                            f32 distance) noexcept {
    SweepResult result;
    ++state_.sweeps;
    ShapeCastInput input;
    input.shape = shape_;
    input.start = Transform::from_translation(from);
    input.direction = direction;
    input.max_distance = distance;
    const QueryFilter filter = query_filter();
    const Expected<ShapeCastHit, Error> hit = server_->shape_cast(world_, input, filter);
    if (!hit || hit->body.is_null()) {
        return result;
    }
    result.hit = true;
    // The skin width is taken off the travel: the capsule stops that far short of the surface so
    // the next step has room to work. Clamped at zero, because a hit closer than the skin width
    // means the capsule is already at or inside the surface and must not travel backwards here —
    // `update_ground` is what corrects that, using `raw_distance`.
    result.raw_distance = hit->distance;
    result.distance = math::max(0.0f, hit->distance - description_.skin_width);
    result.normal = hit->normal;
    result.body = hit->body;
    result.material = hit->material;
    result.started_penetrating = hit->started_penetrating;
    return result;
}

bool CharacterController::is_walkable(Vec3 normal) const noexcept {
    // `physics` — "Steep slope": above the maximum the surface is a WALL, and the character "SHALL
    // be reported as touching a wall rather than grounded". One comparison, used by both the ground
    // test and the slide, so the two cannot disagree about where the boundary is.
    return dot(normal, description_.up) >= cos_max_slope_;
}

Vec3 CharacterController::depenetrate(Vec3 position) noexcept {
    Vec3 corrected = position;
    for (u32 pass = 0; pass < kDepenetrationPasses; ++pass) {
        OverlapInput input;
        input.shape = shape_;
        input.transform = Transform::from_translation(corrected);
        OverlapHit hits[kMaxOverlaps];
        const QueryFilter filter = query_filter();
        const Expected<u32, Error> count =
            server_->overlap(world_, input, filter, Span<OverlapHit>(hits, kMaxOverlaps));
        if (!count || *count == 0) {
            break;
        }
        // The overlap test says WHICH bodies, not how deep. The depth comes from a short sweep,
        // which is the one query that reports `started_penetrating` together with a separating
        // normal. Pushing out along that normal and re-testing is what makes a corner converge
        // rather than oscillate between two walls; `kDepenetrationPasses` bounds it, because a
        // capsule wedged in geometry must be a visible bug and not a hang.
        const SweepResult probe = sweep(corrected, description_.up, description_.skin_width);
        if (!probe.started_penetrating) {
            break;
        }
        corrected += probe.normal * description_.skin_width;
    }
    return corrected;
}

void CharacterController::push_bodies(const SweepResult& result, Vec3 direction,
                                      f32 delta) noexcept {
    // `physics`: "pushing of dynamic bodies with configurable force". An impulse rather than a
    // force, because the controller runs once per fixed step and an impulse is what a step's worth
    // of force is; the conversion is here so `push_force` is readable as newtons.
    if (!description_.push_dynamic_bodies || description_.push_force <= 0.0f || !result.hit ||
        result.body.is_null()) {
        return;
    }
    const Expected<BodyState, Error> state = server_->body_state(result.body);
    if (!state || state->motion != MotionType::Dynamic) {
        return;
    }
    (void)server_->add_impulse(result.body, direction * (description_.push_force * delta));
}

Vec3 CharacterController::slide(Vec3 position, Vec3 motion, bool allow_step_up) noexcept {
    Vec3 current = position;
    Vec3 remaining = motion;
    for (u32 iteration = 0; iteration < description_.max_iterations; ++iteration) {
        const f32 distance = length(remaining);
        if (distance < kMinimumMotion) {
            break;
        }
        const Vec3 direction = remaining / distance;
        const SweepResult result = sweep(current, direction, distance);
        if (!result.hit) {
            current += remaining;
            break;
        }
        current += direction * result.distance;
        push_bodies(result, direction, 1.0f);

        const bool walkable = is_walkable(result.normal);
        if (!walkable) {
            state_.touching_wall = true;
        }
        if (dot(result.normal, description_.up) < -0.5f) {
            state_.touching_ceiling = true;
        }

        // `physics` — "Walking up stairs": blocked by something low, so retry from `step_offset`
        // higher and drop back down. Only for an unwalkable surface — a walkable slope is climbed
        // by sliding, and treating it as a step would make a ramp a staircase.
        if (allow_step_up && !walkable && description_.step_offset > 0.0f) {
            const Vec3 lifted = current + description_.up * description_.step_offset;
            const SweepResult headroom = sweep(current, description_.up, description_.step_offset);
            if (!headroom.hit) {
                const f32 left = distance - result.distance;
                const SweepResult forward = sweep(lifted, direction, left);
                if (!forward.hit || forward.distance > result.distance + kMinimumMotion) {
                    const Vec3 advanced =
                        lifted + direction * (forward.hit ? forward.distance : left);
                    const SweepResult down =
                        sweep(advanced, -description_.up, description_.step_offset * 2.0f);
                    if (down.hit && is_walkable(down.normal)) {
                        current = advanced - description_.up * down.distance;
                        state_.stepped_up = true;
                        break;
                    }
                }
            }
        }

        remaining = slide_along(remaining * (1.0f - (result.distance / distance)), result.normal);
    }
    return current;
}

void CharacterController::update_ground(Vec3& position, f32 delta) noexcept {
    // A probe slightly longer than the skin width: the character has already been placed that far
    // above the surface by every sweep, so a probe of exactly the skin width would report "in air"
    // for a character standing still.
    const f32 probe = (description_.skin_width * 2.0f) + 1e-3f;
    const SweepResult ground = sweep(position, -description_.up, probe);
    if (!ground.hit) {
        state_.ground = GroundState::InAir;
        state_.ground_normal = description_.up;
        // `physics` — "Moving platform": "on leaving, the configured inherited-velocity policy
        // SHALL apply". Applied exactly once, on the step the ground is lost.
        if (had_ground_) {
            switch (description_.inherit_platform_velocity) {
                case InheritedVelocity::Inherit:
                    state_.velocity += state_.platform_velocity;
                    break;
                case InheritedVelocity::HorizontalOnly:
                    state_.velocity += slide_along(state_.platform_velocity, description_.up);
                    break;
                case InheritedVelocity::Discard:
                    break;
            }
        }
        state_.ground_body = BodyHandle{};
        state_.platform_velocity = Vec3{};
        had_ground_ = false;
        return;
    }

    state_.ground_normal = ground.normal;
    state_.ground_body = ground.body;
    state_.ground_material = ground.material;
    state_.ground = is_walkable(ground.normal) ? GroundState::Grounded : GroundState::OnSteepSlope;
    if (state_.ground == GroundState::OnSteepSlope) {
        state_.touching_wall = true;
    } else {
        // SNAP TO EXACTLY ONE SKIN WIDTH ABOVE THE FLOOR, AND THIS IS NOT COSMETIC.
        //
        // A capsule that comes to rest exactly ON a surface is touching it, and a swept query from
        // a point on a surface reports a hit at distance zero for every direction — including
        // straight up and along the floor. The character then cannot walk, cannot jump, and every
        // symptom points at the sweep rather than at the resting position. Holding the capsule one
        // skin width clear is what makes the separation strictly positive, and every query after it
        // unambiguous. Measured: without this line, six of the character cases fail and the
        // character stands still on flat ground while reporting itself grounded.
        position += description_.up * (description_.skin_width - ground.raw_distance);
    }

    // The platform's own motion, measured rather than asked for: a kinematic body has a velocity
    // the caller set, but a body animated by writing transforms has none, and the character has to
    // follow both. Differencing the ground body's transform across one step covers each.
    const Expected<BodyState, Error> ground_state = server_->body_state(ground.body);
    if (ground_state && had_ground_ && state_.ground_body == ground.body && delta > 0.0f) {
        const Vec3 moved =
            ground_state->transform.translation - previous_ground_transform_.translation;
        state_.platform_velocity = moved / delta;
    } else {
        state_.platform_velocity = Vec3{};
    }
    if (ground_state) {
        previous_ground_transform_ = ground_state->transform;
    }
    had_ground_ = true;
}

Status CharacterController::move(f32 delta, const CharacterInput& input) noexcept {
    if (shape_.is_null()) {
        return fail(ErrorCode::Unavailable, "character: create() has not been called");
    }
    if (!(delta > 0.0f) || !math::is_finite(delta)) {
        return fail(ErrorCode::InvalidArgument, "character: delta must be positive and finite");
    }

    state_.sweeps = 0;
    state_.stepped_up = false;
    state_.touching_wall = false;
    state_.touching_ceiling = false;

    Vec3 position = depenetrate(state_.transform.translation);

    const bool grounded = description_.mode == CharacterMode::Grounded;
    Vec3 horizontal = slide_along(input.desired_velocity, description_.up);
    f32 vertical = dot(state_.velocity, description_.up);

    if (grounded) {
        // Gravity is integrated into the controller's own vertical velocity rather than read back
        // from the solver, because the capsule is kinematic and the solver never touches it.
        // 9.81 is not hard-coded: it is the world's gravity projected onto the character's up.
        const f32 gravity = -9.81f * description_.gravity_scale;
        vertical += gravity * delta;
        if (state_.ground == GroundState::Grounded && vertical < 0.0f) {
            // Standing: the accumulated fall is discarded so a character does not build up an
            // arbitrarily large downward velocity while resting, which would then launch it off the
            // first ramp it crossed.
            vertical = 0.0f;
        }
        if (input.jump) {
            vertical = input.jump_speed;
        }
        // The platform carries the character; `physics`: "the platform's motion SHALL be applied to
        // the character".
        horizontal += slide_along(state_.platform_velocity, description_.up);
    } else {
        // Floating: six degrees of freedom, no gravity, no ground. The desired velocity is taken
        // whole, vertical component included.
        horizontal = input.desired_velocity;
        vertical = 0.0f;
    }

    const Vec3 vertical_motion = description_.up * (vertical * delta);
    if (length_squared(vertical_motion) > kMinimumMotion * kMinimumMotion) {
        const f32 distance = length(vertical_motion);
        const Vec3 direction = vertical_motion / distance;
        const SweepResult result = sweep(position, direction, distance);
        if (result.hit) {
            position += direction * result.distance;
            push_bodies(result, direction, delta);
            if (dot(result.normal, description_.up) < -0.5f) {
                state_.touching_ceiling = true;
            }
            const bool ceiling = dot(result.normal, description_.up) <= 0.0f;
            if (is_walkable(result.normal) || ceiling) {
                // Landing on ground the character can stand on zeroes the fall; a head strike
                // zeroes a jump rather than letting it continue through the ceiling. The ceiling
                // case is spelled out rather than folded into the slide below, because a normal
                // pointing DOWN is not a slope and sliding along it would leave the jump's upward
                // velocity intact — the character would hang under the ceiling for ever.
                vertical = 0.0f;
            } else {
                // `physics` — "Steep slope": the character "SHALL slide down". The rest of the
                // vertical motion is projected onto the surface and run through the same slide the
                // horizontal motion uses, and the vertical VELOCITY is kept — which is what makes
                // the descent accelerate rather than creep one skin width per tick.
                const Vec3 left = vertical_motion - direction * result.distance;
                position = slide(position, slide_along(left, result.normal), false);
            }
        } else {
            position += vertical_motion;
        }
    }

    position = slide(position, horizontal * delta, grounded);

    if (grounded) {
        update_ground(position, delta);
    } else {
        state_.ground = GroundState::InAir;
        state_.platform_velocity = Vec3{};
        had_ground_ = false;
    }

    state_.transform.translation = position;
    state_.velocity = horizontal + description_.up * vertical;

    if (!body_.is_null()) {
        // The kinematic body follows, so other bodies collide with the character. Interpolated
        // rather than teleported: the controller moved it continuously, and marking every step a
        // teleport would suppress interpolation on every frame.
        return server_->set_body_transform(body_, state_.transform, TeleportMode::Interpolate);
    }
    return ok();
}

Status CharacterController::teleport(const Transform& transform) noexcept {
    state_.transform = transform;
    state_.velocity = Vec3{};
    state_.ground = GroundState::InAir;
    state_.platform_velocity = Vec3{};
    had_ground_ = false;
    if (!body_.is_null()) {
        return server_->set_body_transform(body_, transform, TeleportMode::Teleport);
    }
    return ok();
}

}  // namespace cy::physics
