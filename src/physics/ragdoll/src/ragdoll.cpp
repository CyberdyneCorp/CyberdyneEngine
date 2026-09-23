// SPDX-License-Identifier: MIT
#include <cy/physics/ragdoll/ragdoll.h>

#include <algorithm>
#include <cmath>

namespace cy::physics::ragdoll {
namespace {

[[nodiscard]] bool valid_weight(f32 weight) noexcept {
    return std::isfinite(weight) && weight >= 0.0f && weight <= 1.0f;
}

[[nodiscard]] bool valid_bone(const BoneProfile& bone) noexcept {
    // The profile stores descriptions by value; array-backed shapes would retain caller pointers.
    const bool owned_shape = bone.shape.type == ShapeType::Sphere ||
                             bone.shape.type == ShapeType::Capsule ||
                             bone.shape.type == ShapeType::Box;
    const bool limits = std::isfinite(bone.twist_limit.min) &&
                        std::isfinite(bone.twist_limit.max) && std::isfinite(bone.swing_limit_y) &&
                        bone.swing_limit_y >= 0.0f && bone.swing_limit_y <= math::kPi &&
                        std::isfinite(bone.swing_limit_z) && bone.swing_limit_z >= 0.0f &&
                        bone.swing_limit_z <= math::kPi &&
                        (!bone.twist_limit.limited() ||
                         (bone.twist_limit.min >= -math::kPi && bone.twist_limit.max <= math::kPi));
    const bool motor = std::isfinite(bone.motor_torque) && bone.motor_torque >= 0.0f &&
                       std::isfinite(bone.motor_frequency) && bone.motor_frequency >= 0.0f;
    const Transform& local = bone.collider_local;
    const f32 rotation_length = length_squared(local.rotation);
    const bool collider =
        std::isfinite(local.translation.x) && std::isfinite(local.translation.y) &&
        std::isfinite(local.translation.z) && std::isfinite(rotation_length) &&
        std::fabs(rotation_length - 1.0f) < 0.01f && local.scale == Vec3{1.0f, 1.0f, 1.0f};
    return owned_shape && std::isfinite(bone.mass) && bone.mass > 0.0f && validate(bone.shape) &&
           limits && motor && collider;
}

[[nodiscard]] Vec3 angular_velocity(Quat current, Quat previous, f32 delta_seconds) noexcept {
    const Quat delta = normalize(current * inverse(previous));
    Vec3 axis;
    f32 angle = 0.0f;
    delta.to_axis_angle(axis, angle);
    if (angle > math::kPi) {
        angle -= 2.0f * math::kPi;
    }
    return axis * (angle / delta_seconds);
}

template <typename Handle, typename Destroy>
[[nodiscard]] Status destroy_handles(Span<Handle> handles, Destroy&& destroy) noexcept {
    Status first = ok();
    for (Handle handle : handles) {
        if (handle.is_null()) {
            continue;
        }
        if (Status result = destroy(handle); !result && first) {
            first = result;
        }
    }
    return first;
}

}  // namespace

Ragdoll::Ragdoll(PhysicsServer& server, WorldHandle world, const animation::Skeleton& skeleton,
                 const Profile& profile, Allocator& allocator) noexcept
    : server_(server),
      world_(world),
      skeleton_(skeleton),
      profile_(profile),
      shapes_(allocator),
      bodies_(allocator),
      joints_(allocator),
      blends_(allocator) {}

Ragdoll::~Ragdoll() {
    (void)deactivate();
}

Status Ragdoll::activate(Span<const Transform> current, Span<const Transform> previous,
                         const Transform& actor_world, const Transform& previous_actor_world,
                         f32 delta_seconds, const Activation& activation) noexcept {
    const u16 count = skeleton_.joint_count();
    if (active_ || !bodies_.empty()) {
        return fail(ErrorCode::InvalidArgument, "ragdoll: already active");
    }
    if (!server_.capabilities().constraints) {
        return fail(ErrorCode::Unsupported, "ragdoll: backend does not support constraints");
    }
    if (Status valid = validate_activation(current, previous, delta_seconds, activation); !valid) {
        return valid;
    }
    mode_ = activation.mode;
    if (Status sized = blends_.resize(count); !sized) {
        return sized;
    }
    for (u16 index = 0; index < count; ++index) {
        f32 target = mode_ == Mode::Full ? 1.0f : 0.0f;
        if (mode_ == Mode::Partial) {
            target = activation.partial_weights[index];
        }
        blends_[index].base = target;
        start_blend(blends_[index], target, activation.blend_seconds);
    }
    if (Status created =
            create_bodies(current, previous, actor_world, previous_actor_world, delta_seconds);
        !created) {
        (void)deactivate();
        return created;
    }
    if (Status created = create_joints(current, actor_world); !created) {
        (void)deactivate();
        return created;
    }
    active_ = true;
    if (Status motors = initialize_motors(current, actor_world); !motors) {
        (void)deactivate();
        return motors;
    }
    return ok();
}

Status Ragdoll::initialize_motors(Span<const Transform> model_pose,
                                  const Transform& actor_world) noexcept {
    if (mode_ == Mode::Full) {
        return ok();
    }
    for (u16 index = 0; index < skeleton_.joint_count(); ++index) {
        if (Status driven = drive_joint(index, model_pose, actor_world); !driven) {
            return driven;
        }
    }
    return ok();
}

Status Ragdoll::validate_activation(Span<const Transform> current, Span<const Transform> previous,
                                    f32 delta_seconds,
                                    const Activation& activation) const noexcept {
    const u16 count = skeleton_.joint_count();
    if (!skeleton_.finalized() || profile_.bones().size() != count || current.size() != count ||
        previous.size() != count || !std::isfinite(delta_seconds) || delta_seconds <= 0.0f ||
        !std::isfinite(activation.blend_seconds) || activation.blend_seconds < 0.0f ||
        (activation.mode == Mode::Partial && activation.partial_weights.size() != count)) {
        return fail(ErrorCode::InvalidArgument, "ragdoll: invalid activation inputs");
    }
    for (u16 index = 0; index < count; ++index) {
        const BoneProfile& bone = profile_.bones()[index];
        if (bone.parent != skeleton_.joints()[index].parent || !valid_bone(bone) ||
            (activation.mode == Mode::Partial &&
             !valid_weight(activation.partial_weights[index]))) {
            return fail(ErrorCode::InvalidArgument, "ragdoll: invalid bone profile or weight");
        }
    }
    return ok();
}

Status Ragdoll::create_bodies(Span<const Transform> current, Span<const Transform> previous,
                              const Transform& actor_world, const Transform& previous_actor_world,
                              f32 delta_seconds) noexcept {
    const u16 count = skeleton_.joint_count();
    if (Status reserved = shapes_.reserve(count); !reserved) {
        return reserved;
    }
    if (Status reserved = bodies_.reserve(count); !reserved) {
        return reserved;
    }
    for (u16 index = 0; index < count; ++index) {
        const BoneProfile& bone = profile_.bones()[index];
        const auto shape = server_.create_shape(bone.shape);
        if (!shape) {
            return make_unexpected(shape.error());
        }
        if (Status added = shapes_.push_back(*shape); !added) {
            (void)server_.destroy_shape(*shape);
            return added;
        }
        ColliderDescription collider;
        collider.shape = *shape;
        collider.local = bone.collider_local;
        BodyDescription body;
        body.name = skeleton_.joints()[index].name;
        body.motion = dynamic_bone(index) ? MotionType::Dynamic : MotionType::Kinematic;
        body.transform = actor_world * current[index];
        const Transform last = previous_actor_world * previous[index];
        body.linear_velocity = (body.transform.translation - last.translation) / delta_seconds;
        body.angular_velocity =
            angular_velocity(body.transform.rotation, last.rotation, delta_seconds);
        body.mass = bone.mass;
        body.colliders = &collider;
        body.collider_count = 1;
        const auto created = server_.create_body(world_, body);
        if (!created) {
            return make_unexpected(created.error());
        }
        if (Status added = bodies_.push_back(*created); !added) {
            (void)server_.destroy_body(*created);
            return added;
        }
    }
    return ok();
}

bool Ragdoll::dynamic_bone(u16 joint) const noexcept {
    if (mode_ == Mode::Full) {
        return true;
    }
    if (mode_ == Mode::Powered) {
        return profile_.bones()[joint].parent != animation::kInvalidJoint;
    }
    return blends_[joint].base > 0.0f;
}

Status Ragdoll::create_joints(Span<const Transform> current,
                              const Transform& actor_world) noexcept {
    const u16 count = skeleton_.joint_count();
    if (Status sized = joints_.resize(count); !sized) {
        return sized;
    }
    for (u16 index = 0; index < count; ++index) {
        const BoneProfile& bone = profile_.bones()[index];
        if (bone.parent == animation::kInvalidJoint) {
            continue;
        }
        const Transform parent_world = actor_world * current[bone.parent];
        const Transform child_world = actor_world * current[index];
        ConstraintDescription joint;
        joint.type = ConstraintType::SwingTwist;
        joint.body_a = bodies_[bone.parent];
        joint.body_b = bodies_[index];
        joint.frame_a.translation = inverse(parent_world).transform_point(child_world.translation);
        joint.frame_a.rotation = inverse(parent_world.rotation) * child_world.rotation;
        joint.swing_limit_y = bone.swing_limit_y;
        joint.swing_limit_z = bone.swing_limit_z;
        joint.twist_limit = bone.twist_limit;
        const auto created = server_.create_constraint(world_, joint);
        if (!created) {
            return make_unexpected(created.error());
        }
        joints_[index] = *created;
    }
    return ok();
}

Status Ragdoll::deactivate() noexcept {
    Status result = destroy_handles(joints_.span(), [this](ConstraintHandle handle) noexcept {
        return server_.destroy_constraint(handle);
    });
    Status bodies = destroy_handles(bodies_.span(), [this](BodyHandle handle) noexcept {
        return server_.destroy_body(handle);
    });
    Status shapes = destroy_handles(shapes_.span(), [this](ShapeHandle handle) noexcept {
        return server_.destroy_shape(handle);
    });
    if (result && !bodies) {
        result = bodies;
    }
    if (result && !shapes) {
        result = shapes;
    }
    joints_.clear();
    bodies_.clear();
    shapes_.clear();
    blends_.clear();
    active_ = false;
    return result;
}

Status Ragdoll::set_animation_target(Span<const Transform> model_pose, const Transform& actor_world,
                                     f32 delta_seconds) noexcept {
    if (!active_ || model_pose.size() != skeleton_.joint_count() || !std::isfinite(delta_seconds) ||
        delta_seconds <= 0.0f) {
        return fail(ErrorCode::InvalidArgument, "ragdoll: invalid animation target");
    }
    if (mode_ == Mode::Full) {
        return ok();
    }
    for (u16 index = 0; index < skeleton_.joint_count(); ++index) {
        const Transform world_pose = actor_world * model_pose[index];
        if (Status moved = move_kinematic(index, world_pose, delta_seconds); !moved) {
            return moved;
        }
        if (Status driven = drive_joint(index, model_pose, actor_world); !driven) {
            return driven;
        }
    }
    return ok();
}

Status Ragdoll::move_kinematic(u16 joint, const Transform& world_pose, f32 delta_seconds) noexcept {
    const auto state = server_.body_state(bodies_[joint]);
    if (!state) {
        return make_unexpected(state.error());
    }
    if (state->motion != MotionType::Kinematic) {
        return ok();
    }
    const Vec3 linear = (world_pose.translation - state->transform.translation) / delta_seconds;
    const Vec3 angular =
        angular_velocity(world_pose.rotation, state->transform.rotation, delta_seconds);
    if (Status velocity = server_.set_body_velocity(bodies_[joint], linear, angular); !velocity) {
        return velocity;
    }
    return server_.set_body_transform(bodies_[joint], world_pose, TeleportMode::Interpolate);
}

Status Ragdoll::drive_joint(u16 joint, Span<const Transform> model_pose,
                            const Transform& actor_world) noexcept {
    const BoneProfile& bone = profile_.bones()[joint];
    if (bone.parent == animation::kInvalidJoint || joints_[joint].is_null()) {
        return ok();
    }
    OrientationMotorSettings motor;
    const Transform parent_world = actor_world * model_pose[bone.parent];
    const Transform child_world = actor_world * model_pose[joint];
    motor.target_orientation = inverse(parent_world.rotation) * child_world.rotation;
    motor.max_torque = bone.motor_torque;
    motor.spring_frequency = bone.motor_frequency;
    return server_.set_constraint_orientation_motor(joints_[joint], motor);
}

void Ragdoll::start_blend(Blend& blend, f32 target, f32 seconds) noexcept {
    blend.from = blend.weight;
    blend.target = target;
    blend.elapsed = 0.0f;
    blend.duration = seconds;
    if (seconds == 0.0f) {
        blend.weight = target;
    }
}

Status Ragdoll::advance_blend(f32 delta_seconds) noexcept {
    if (!active_ || !std::isfinite(delta_seconds) || delta_seconds < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "ragdoll: invalid blend step");
    }
    for (Blend& blend : blends_) {
        if (blend.duration <= 0.0f) {
            continue;
        }
        blend.elapsed = std::min(blend.duration, blend.elapsed + delta_seconds);
        const f32 alpha = blend.elapsed / blend.duration;
        blend.weight = blend.from + ((blend.target - blend.from) * alpha);
    }
    return ok();
}

Status Ragdoll::sample_pose(Span<const Transform> animation_model, const Transform& actor_world,
                            Span<Transform> out_model) const noexcept {
    const u16 count = skeleton_.joint_count();
    if (!active_ || animation_model.size() != count || out_model.size() != count) {
        return fail(ErrorCode::InvalidArgument, "ragdoll: invalid pose buffer");
    }
    const Transform world_to_actor = inverse(actor_world);
    for (u16 index = 0; index < count; ++index) {
        const auto state = server_.body_state(bodies_[index]);
        if (!state) {
            return make_unexpected(state.error());
        }
        Transform simulated = world_to_actor * state->transform;
        simulated.scale = animation_model[index].scale;
        out_model[index] = interpolate(animation_model[index], simulated, blends_[index].weight);
    }
    return ok();
}

Status Ragdoll::apply_hit(u16 joint, Vec3 impulse, f32 recovery_seconds) noexcept {
    if (!active_ || joint >= skeleton_.joint_count() || !std::isfinite(recovery_seconds) ||
        recovery_seconds < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "ragdoll: invalid hit");
    }
    if (Status applied = server_.add_impulse(bodies_[joint], impulse); !applied) {
        return applied;
    }
    Blend& blend = blends_[joint];
    blend.weight = 1.0f;
    start_blend(blend, blend.base, recovery_seconds);
    return ok();
}

BodyHandle Ragdoll::body(u16 joint) const noexcept {
    return joint < bodies_.size() ? bodies_[joint] : BodyHandle{};
}

f32 Ragdoll::blend_weight(u16 joint) const noexcept {
    return joint < blends_.size() ? blends_[joint].weight : 0.0f;
}

}  // namespace cy::physics::ragdoll
