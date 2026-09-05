// Executing a compiled rig program. See cy/servers/camera/rig.h.
//
// One function per node kind, dispatched by a `switch` over a contiguous array of ops. No virtual
// call, no allocation, no pointer chased per node — `camera-system`: "it SHALL execute a compiled
// program with no per-node allocation or virtual dispatch". The only two things that can allocate
// are the caller's trace array and the caller's query array, and both are optional and both are the
// caller's to size.
//
// WHY THE OPS ARE FREE FUNCTIONS AND NOT MEMBERS. Each one reads the op, the input and the rig's
// state and writes the frame; none of them can reach anything else, so the set of things a node can
// influence is visible in its signature. That is the same reason the frame is a register file
// rather than a pointer to the server.

#include <cy/servers/camera/rig.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::camera {
namespace {

/// Below this, a direction is not a direction and normalising it would produce a NaN.
constexpr f32 kMinDirectionLength = 1e-5F;

/// Wrap an angle into (−pi, pi]. Yaw accumulates for the length of a session, and a float that has
/// grown to a few thousand radians has lost the precision a smooth turn needs.
[[nodiscard]] f32 wrap_pi(f32 radians) noexcept {
    constexpr f32 two_pi = 2.0F * math::kPi;
    f32 wrapped = std::fmod(radians + math::kPi, two_pi);
    if (wrapped < 0.0F) {
        wrapped += two_pi;
    }
    return wrapped - math::kPi;
}

/// `Quat::look_rotation` builds its basis with `cross(up, forward)`, which is the zero vector when
/// the two are parallel — a camera looking straight down. Pick a second up in that case, which is
/// what "level the horizon" degenerates to when there is no horizon.
[[nodiscard]] Quat look_along(Vec3 direction, Vec3 up) noexcept {
    const Vec3 forward = normalize(direction);
    if (std::fabs(dot(forward, up)) > 0.9999F) {
        return Quat::look_rotation(forward, kAxisForward);
    }
    return Quat::look_rotation(forward, up);
}

// --- The nodes ----------------------------------------------------------------------------------

void eval_target(const RigOp& op, const RigEvaluationInput& input, RigState& state,
                 RigFrame& frame) noexcept {
    Vec3 anchor = state.anchor.value();
    if (input.sample.valid) {
        anchor = op.target.use_bounds_center ? input.sample.bounds.center()
                                             : input.sample.transform.translation;
        anchor = anchor + input.sample.transform.rotate_vector(op.target.anchor_offset);
        frame.anchor_velocity = input.sample.velocity;
    } else if (input.binding.kind == TargetKind::Position) {
        anchor = input.binding.position + op.target.anchor_offset;
    } else if (input.binding.kind == TargetKind::Bounds) {
        anchor = input.binding.bounds.center() + op.target.anchor_offset;
    }
    // An unresolved binding of any other kind HOLDS THE LAST ANCHOR rather than snapping to the
    // origin. A target that streamed out for two frames should not throw the camera across the
    // world and back.

    if (!state.primed) {
        state.anchor.reset(anchor);
    }
    frame.anchor = state.anchor.advance(anchor, op.target.anchor_half_life, input.delta_seconds);
    frame.has_anchor = true;
}

void eval_orbit(const RigOp& op, const RigEvaluationInput& input, RigState& state,
                RigFrame& frame) noexcept {
    const OrbitParams& params = op.orbit;

    state.yaw_radians = wrap_pi(state.yaw_radians + (input.intent.look.x * params.yaw_scale));
    state.pitch_radians =
        math::clamp(state.pitch_radians + (input.intent.look.y * params.pitch_scale),
                    params.min_pitch_radians, params.max_pitch_radians);

    // RECENTRE LATCHES. A tap completes the recentre; without the latch a one-frame intent would
    // move the yaw by one frame's worth of the decay and stop, which a designer would discover by
    // holding the button down and concluding the feature was "slow".
    if (input.intent.recentre) {
        state.recentring = true;
    }
    if (state.recentring && input.sample.valid) {
        const Vec3 facing = input.sample.transform.forward();
        const f32 target_yaw = std::atan2(-facing.x, -facing.z);
        const f32 delta = wrap_pi(target_yaw - state.yaw_radians);
        state.yaw_radians =
            wrap_pi(state.yaw_radians +
                    (delta * decay_fraction(params.recentre_half_life, input.delta_seconds)));
        if (std::fabs(delta) < 0.001F) {
            state.recentring = false;
        }
    } else {
        state.recentring = false;
    }

    const f32 distance_target = math::lerp(params.near_distance, params.far_distance, frame.zoom);
    if (!state.primed) {
        state.distance.reset(distance_target);
    }
    const f32 distance =
        state.distance.advance(distance_target, params.distance_half_life, input.delta_seconds);

    // ZOOM IS ONE PARAMETER. Distance and tilt follow the same normalised value through their own
    // curves, so zooming out raises and tilts the camera coherently rather than through two
    // controls that a designer has to keep in step.
    f32 pitch = state.pitch_radians;
    if (params.zoom_drives_pitch) {
        pitch = math::clamp(
            pitch + math::lerp(params.near_pitch_radians, params.far_pitch_radians, frame.zoom),
            params.min_pitch_radians, params.max_pitch_radians);
    }

    const Quat orientation = Quat::from_euler_yxz(Vec3{pitch, state.yaw_radians, 0.0F});
    frame.rotation = orientation;
    frame.yaw_radians = state.yaw_radians;
    frame.pitch_radians = pitch;
    frame.distance = distance;
    frame.position = frame.anchor - (orientation * kAxisForward) * distance;
}

void eval_follow(const RigOp& op, const RigEvaluationInput& input, RigState& state,
                 RigFrame& frame) noexcept {
    const FollowParams& params = op.follow;
    const Vec3 base = frame.has_anchor ? frame.anchor : frame.position;

    Vec3 offset = params.offset;
    switch (params.space) {
        case FollowSpace::World:
            break;
        case FollowSpace::Target:
            if (input.sample.valid) {
                offset = input.sample.transform.rotate_vector(params.offset);
            }
            break;
        case FollowSpace::Rig:
        case FollowSpace::Count:
            offset = frame.rotation * params.offset;
            break;
    }

    Vec3 desired = base + offset;
    if (!state.primed) {
        state.position.reset(desired);
    }
    // THE DEAD ZONE IS ON THE DESIRED POSITION, NOT ON THE OUTPUT. Movement smaller than the dead
    // zone produces no new target, so the camera settles and stays settled; clamping the output
    // instead would leave the smoother chasing a target it is never allowed to reach.
    if (params.dead_zone > 0.0F && length(desired - state.position.value()) <= params.dead_zone) {
        desired = state.position.value();
    }
    frame.position =
        state.position.advance(desired, params.position_half_life, input.delta_seconds);
}

void eval_offset(const RigOp& op, RigFrame& frame) noexcept {
    Vec3 offset = op.offset.offset;
    // The authored mirror and a collision-requested shoulder swap compose: swapping a rig that was
    // already mirrored puts the camera back on the original shoulder, which is what a reader of
    // either flag alone would expect.
    if (op.offset.mirrored != frame.shoulder_swapped) {
        offset.x = -offset.x;
    }
    frame.position = frame.position + (frame.rotation * offset);
}

void eval_look_at(const RigOp& op, const RigEvaluationInput& input, RigState& state,
                  RigFrame& frame) noexcept {
    const LookAtParams& params = op.look_at;
    const Vec3 aim_point =
        (frame.has_anchor ? frame.anchor : frame.position + (frame.rotation * kAxisForward)) +
        params.aim_offset;
    const Vec3 direction = aim_point - frame.position;
    if (length_squared(direction) <= (kMinDirectionLength * kMinDirectionLength)) {
        return;
    }

    // `look_rotation` builds its basis from a world up, so the result has no roll — which IS
    // "level the horizon". `level_horizon == false` is the case that would need a different up, and
    // at Seed there is no node that supplies one, so it takes the same path and says so.
    const Quat desired = look_along(direction, kAxisUp);
    if (!state.primed) {
        state.rotation.reset(desired);
    }
    frame.rotation =
        state.rotation.advance(desired, params.rotation_half_life, input.delta_seconds);
}

void eval_lens(const RigOp& op, const RigEvaluationInput& input, RigState& state,
               RigFrame& frame) noexcept {
    const LensParams& params = op.lens;
    const f32 target = math::lerp(params.near_value, params.far_value, frame.zoom);
    if (!state.primed) {
        state.lens_value.reset(target);
    }
    const f32 value = state.lens_value.advance(target, params.half_life, input.delta_seconds);

    frame.lens.kind = params.kind;
    frame.lens.projection = params.projection;
    frame.lens.gameplay.near_plane = params.near_plane;
    frame.lens.gameplay.far_plane = params.far_plane;
    frame.lens.ortho_height = params.ortho_height;
    frame.lens.physical.sensor_height_mm = params.sensor_height_mm;

    // Both halves are written whichever one was authored, so a consumer reading the other never
    // sees a stale value. lens.h's `vertical_fov_radians()` derives rather than reads, so the two
    // cannot disagree even if this changed.
    if (params.kind == LensKind::Physical) {
        frame.lens.physical.focal_length_mm = value;
        frame.lens.gameplay.vertical_fov_radians =
            fov_from_focal_length(value, params.sensor_height_mm);
    } else {
        frame.lens.gameplay.vertical_fov_radians = value;
        frame.lens.physical.focal_length_mm = focal_length_from_fov(value, params.sensor_height_mm);
    }
}

void eval_noise(const RigOp& op, const RigEvaluationInput& input, const RigState& state,
                RigFrame& frame) noexcept {
    const NoiseParams& params = op.noise;
    Vec3 offset{0.0F, 0.0F, 0.0F};
    Vec3 euler{0.0F, 0.0F, 0.0F};

    for (const CameraImpulse& impulse : input.impulses) {
        if (!params.tag.is_empty() && impulse.tag != params.tag) {
            continue;
        }
        // Three sine oscillators at incommensurate rates rather than a random number: a shake has
        // to be reproducible from the tick for a replay to present identically, and a seeded
        // generator would need its state carried through the reload boundary as well.
        const f32 omega = 2.0F * math::kPi * impulse.frequency_hz;
        const f32 time = state.noise_time;
        offset = offset + (Vec3{std::sin(omega * time), std::sin((omega * 1.37F * time) + 1.7F),
                                std::sin((omega * 0.73F * time) + 3.1F)} *
                           (impulse.strength * params.position_scale));
        euler = euler + (Vec3{std::sin((omega * 1.11F * time) + 0.5F),
                              std::sin((omega * 0.89F * time) + 2.2F),
                              std::sin((omega * 1.23F * time) + 4.4F)} *
                         (impulse.strength * params.rotation_scale));
    }

    // The player's shake setting is applied HERE and nowhere else. `camera-system`: "**WHEN** a
    // player reduces camera shake **THEN** every shake source SHALL scale, with no per-effect
    // handling."
    const f32 scale = params.amplitude * input.shake_scale;
    frame.shake_offset = frame.shake_offset + (offset * scale);
    frame.shake_rotation = frame.shake_rotation * Quat::from_euler_yxz(euler * scale);
}

void eval_constraint(const RigOp& op, RigFrame& frame) noexcept {
    const ConstraintParams& params = op.constraint;

    if (frame.has_anchor && (params.min_distance > 0.0F || params.max_distance > 0.0F)) {
        const Vec3 offset = frame.position - frame.anchor;
        const f32 distance = length(offset);
        if (distance > kMinDirectionLength) {
            f32 clamped = distance;
            if (params.max_distance > 0.0F) {
                clamped = math::min(clamped, params.max_distance);
            }
            clamped = math::max(clamped, params.min_distance);
            frame.position = frame.anchor + (offset * (clamped / distance));
        }
    }

    // A zero extent on an axis leaves that axis unconstrained, so a side-scroller constrains one
    // axis and a bounded map two, from this one node rather than from three camera types.
    for (usize axis = 0; axis < 3; ++axis) {
        const f32 extent = params.region_extents[axis];
        if (extent <= 0.0F) {
            continue;
        }
        const f32 center = params.region_center[axis];
        frame.position[axis] = math::clamp(frame.position[axis], center - extent, center + extent);
    }
}

/// The worst (smallest) collision fraction and the worst occlusion this rig was given.
struct QueryAnswers {
    f32 collision_fraction = 1.0F;
    Vec3 collision_normal{0.0F, 1.0F, 0.0F};
    f32 occlusion_blocked = 0.0F;
    bool occlusion_transparent = false;
};

[[nodiscard]] QueryAnswers gather_answers(const RigEvaluationInput& input) noexcept {
    QueryAnswers answers;
    for (const CameraQueryResult& result : input.query_results) {
        if (result.rig_bits != input.rig_bits) {
            continue;
        }
        if (result.kind == CameraQuery::Kind::Collision) {
            // A TRANSPARENT HIT IS NOT A COLLISION. "Glass occludes without colliding": a
            // transparent surface reported on the collision query is dropped here and answered by
            // the occlusion response instead. This one branch is the whole of that requirement.
            if (!result.transparent && result.fraction < answers.collision_fraction) {
                answers.collision_fraction = result.fraction;
                answers.collision_normal = result.normal;
            }
        } else {
            answers.occlusion_blocked =
                math::max(answers.occlusion_blocked, result.blocked_fraction);
            answers.occlusion_transparent = answers.occlusion_transparent || result.transparent;
        }
    }
    return answers;
}

/// Apply one response at `fraction` of the way from the anchor to the desired position.
void apply_response(CollisionResponse response, f32 fraction, Vec3 normal, const RigOp& op,
                    const RigEvaluationInput& input, RigState& state, RigFrame& frame) noexcept {
    if (fraction >= 1.0F || !frame.has_anchor) {
        return;
    }
    const Vec3 offset = frame.position - frame.anchor;
    const f32 distance = length(offset);
    if (distance <= kMinDirectionLength) {
        return;
    }
    const Vec3 direction = offset * (1.0F / distance);

    switch (response) {
        case CollisionResponse::PullIn: {
            // The accessibility setting scales how far the camera is allowed to be pushed, which is
            // `camera-system`'s "adjustable camera collision strength".
            const f32 allowed = math::lerp(distance, distance * fraction, input.collision_strength);
            if (!state.primed) {
                state.collision_distance.reset(allowed);
            }
            // IN IMMEDIATELY, OUT SLOWLY. A camera that eased into a wall would clip through it for
            // the duration of the ease; a camera that snapped back out the moment the wall cleared
            // would pop. Only the recovery is smoothed.
            const f32 current = state.collision_distance.value();
            const f32 next = (allowed < current) ? allowed
                                                 : state.collision_distance.advance(
                                                       allowed, op.collision.recovery_half_life,
                                                       input.delta_seconds);
            state.collision_distance.reset(next);
            frame.position = frame.anchor + (direction * next);
            break;
        }
        case CollisionResponse::Slide:
            // Stand off the surface by the probe radius rather than pulling toward the anchor, so a
            // camera brushing a wall slides along it instead of diving at the target.
            frame.position =
                frame.anchor + (offset * fraction) + (normal * op.collision.probe_radius);
            break;
        case CollisionResponse::SwapShoulder:
            // Takes effect on the NEXT evaluation: the `Offset` node that reads it runs before this
            // one. That is the correct order — swapping shoulders mid-frame would move the camera
            // without a blend — and the flag lives in the frame so the swap is visible to the
            // trace.
            frame.shoulder_swapped = true;
            break;
        case CollisionResponse::FadeObstacle:
            // The camera does NOT move. Whoever owns the obstacle's material is asked to fade it.
            frame.obstacle_fade = math::max(frame.obstacle_fade, 1.0F - fraction);
            break;
        case CollisionResponse::Ignore:
        case CollisionResponse::Count:
            break;
    }
}

void eval_collision(const RigOp& op, const RigEvaluationInput& input, RigState& state,
                    RigFrame& frame) noexcept {
    const QueryAnswers answers = gather_answers(input);

    // COLLISION AND OCCLUSION ARE DIFFERENT QUESTIONS WITH DIFFERENT RESPONSES, which is why they
    // are two calls with two responses and not one number.
    apply_response(op.collision.collision_response, answers.collision_fraction,
                   answers.collision_normal, op, input, state, frame);
    if (answers.occlusion_blocked > 0.0F) {
        apply_response(op.collision.occlusion_response, 1.0F - answers.occlusion_blocked,
                       answers.collision_normal, op, input, state, frame);
    }

    // Publish the queries for the next evaluation. APPENDED TO A BATCH, never cast: `camera-system`
    // requires queries to be "batched through the physics interface", and this module is at layer 2
    // and could not reach a physics server even if it wanted to.
    if (input.queries == nullptr || !frame.has_anchor) {
        return;
    }
    CameraQuery collision;
    collision.kind = CameraQuery::Kind::Collision;
    collision.rig_bits = input.rig_bits;
    collision.from = frame.anchor;
    collision.to = frame.position;
    collision.radius = op.collision.probe_radius;
    (void)input.queries->push_back(collision);

    if (op.collision.occlusion_samples > 0) {
        CameraQuery occlusion;
        occlusion.kind = CameraQuery::Kind::Occlusion;
        occlusion.rig_bits = input.rig_bits;
        occlusion.from = frame.position;
        occlusion.to = frame.anchor;
        (void)input.queries->push_back(occlusion);
    }
}

/// Execute one op. Extracted so `evaluate()` is a loop with a trace around it rather than a
/// three-hundred-line switch.
void execute(const RigOp& op, const RigNodeRegistry& registry, const RigEvaluationInput& input,
             RigState& state, RigFrame& frame) noexcept {
    switch (op.kind) {
        case RigNodeKind::Target:
            eval_target(op, input, state, frame);
            break;
        case RigNodeKind::Follow:
            eval_follow(op, input, state, frame);
            break;
        case RigNodeKind::Orbit:
            eval_orbit(op, input, state, frame);
            break;
        case RigNodeKind::Offset:
            eval_offset(op, frame);
            break;
        case RigNodeKind::LookAt:
            eval_look_at(op, input, state, frame);
            break;
        case RigNodeKind::Lens:
            eval_lens(op, input, state, frame);
            break;
        case RigNodeKind::Noise:
            eval_noise(op, input, state, frame);
            break;
        case RigNodeKind::Constraint:
            eval_constraint(op, frame);
            break;
        case RigNodeKind::Collision:
            eval_collision(op, input, state, frame);
            break;
        case RigNodeKind::Custom:
            registry.evaluate(op.custom_op, op, input, frame);
            break;
        case RigNodeKind::Output:
        case RigNodeKind::Count:
            break;
    }
}

[[nodiscard]] RigOpTrace begin_trace(const RigOp& op, const RigFrame& frame) noexcept {
    RigOpTrace entry;
    entry.id = op.id;
    entry.kind = op.kind;
    entry.position_before = frame.position;
    entry.fov_before_radians = frame.lens.vertical_fov_radians();
    return entry;
}

void end_trace(RigOpTrace& entry, const Quat& rotation_before, const RigFrame& frame) noexcept {
    entry.position_after = frame.position;
    entry.fov_after_radians = frame.lens.vertical_fov_radians();
    // The angle between two orientations, from the dot product of their quaternions. `fabs` folds
    // the double cover, so a rotation and its negation read as the same orientation rather than as
    // a half turn apart.
    const f32 cosine = math::clamp(
        std::fabs((rotation_before.x * frame.rotation.x) + (rotation_before.y * frame.rotation.y) +
                  (rotation_before.z * frame.rotation.z) + (rotation_before.w * frame.rotation.w)),
        0.0F, 1.0F);
    entry.rotation_delta_radians = 2.0F * std::acos(cosine);
}

}  // namespace

void RigState::reset(const Transform& pose, const Lens& lens) noexcept {
    anchor.reset(pose.translation);
    position.reset(pose.translation);
    rotation.reset(pose.rotation);
    distance.reset(0.0F);
    lens_value.reset(lens.kind == LensKind::Physical ? lens.physical.focal_length_mm
                                                     : lens.gameplay.vertical_fov_radians);
    collision_distance.reset(0.0F);
    recentring = false;
    // `primed` is NOT set: a reset rig re-primes on its next evaluation, which is what makes a cut
    // snap to the new pose instead of easing out of the old one.
    primed = false;
    previous_position = pose.translation;
    has_previous = false;
}

Status evaluate(const RigProgram& program, const RigNodeRegistry& registry, RigState& state,
                const RigEvaluationInput& input, RigFrame& frame,
                Array<RigOpTrace>* trace) noexcept {
    if (program.ops.empty()) {
        return fail(ErrorCode::InvalidArgument, "a compiled rig program has no ops");
    }

    // The prologue, done once rather than by whichever node happens to need it first: the zoom is a
    // rig-wide parameter that both the orbit and the lens read, and the noise clock has to advance
    // whether or not there is a noise node.
    if (input.intent.zoom_set) {
        state.zoom = math::clamp(input.intent.zoom, 0.0F, 1.0F);
    }
    state.noise_time += input.delta_seconds;
    frame.zoom = state.zoom;

    if (trace != nullptr) {
        trace->clear();
        if (Status reserved = trace->reserve(program.ops.size()); !reserved) {
            return reserved;
        }
    }

    for (const RigOp& op : program.ops) {
        if (trace == nullptr) {
            execute(op, registry, input, state, frame);
            continue;
        }
        RigOpTrace entry = begin_trace(op, frame);
        const Quat rotation_before = frame.rotation;
        execute(op, registry, input, state, frame);
        end_trace(entry, rotation_before, frame);
        if (Status pushed = trace->push_back(entry); !pushed) {
            return pushed;
        }
    }

    state.primed = true;
    return ok();
}

}  // namespace cy::camera
