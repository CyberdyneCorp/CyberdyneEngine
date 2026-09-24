// SPDX-License-Identifier: MIT
#pragma once
// Ragdoll profiles and simulation live at the animation/physics join, never in a backend.

#include <cy/animation/skeleton.h>
#include <cy/core/memory/array.h>
#include <cy/servers/physics/server.h>

namespace cy::physics::ragdoll {

/// Editable asset data. One bone per skeleton joint, in the same parent-before-child order.
struct BoneProfile {
    u16 parent = animation::kInvalidJoint;
    /// Primitive only (sphere, capsule or box); array-backed shapes would borrow caller memory.
    ShapeDescription shape;
    Transform collider_local;
    f32 mass = 1.0f;
    f32 swing_limit_y = 0.7f;
    f32 swing_limit_z = 0.7f;
    AxisLimit twist_limit{-0.5f, 0.5f};
    f32 motor_torque = 60.0f;
    f32 motor_frequency = 8.0f;
};

class Profile {
public:
    explicit Profile(Allocator& allocator) noexcept : bones_(allocator) {}

    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
    Profile(Profile&&) noexcept = default;
    Profile& operator=(Profile&&) noexcept = default;

    /// Generate usable body shapes, masses and constrained joints from a finalized skeleton.
    [[nodiscard]] static Expected<Profile, Error> generate(const animation::Skeleton& skeleton,
                                                           Allocator& allocator) noexcept;
    [[nodiscard]] Span<BoneProfile> bones() noexcept { return bones_.span(); }
    [[nodiscard]] Span<const BoneProfile> bones() const noexcept { return bones_.span(); }

private:
    Array<BoneProfile> bones_;
};

enum class Mode : u8 { Full, Powered, Partial };

struct Activation {
    Mode mode = Mode::Full;
    /// Partial mode only: one blend target per bone in [0, 1]. Zero remains animation-driven.
    Span<const f32> partial_weights;
    f32 blend_seconds = 0.0f;
};

/// Owns solver handles, but neither the physics server/world nor the skeleton/profile asset.
/// Destroy it before destroying its physics world.
class Ragdoll {
public:
    Ragdoll(PhysicsServer& server, WorldHandle world, const animation::Skeleton& skeleton,
            const Profile& profile, Allocator& allocator) noexcept;
    ~Ragdoll();

    Ragdoll(const Ragdoll&) = delete;
    Ragdoll& operator=(const Ragdoll&) = delete;

    /// Pose inputs are model-space and indexed by skeleton joint. Previous pose seeds velocity.
    [[nodiscard]] Status activate(Span<const Transform> current, Span<const Transform> previous,
                                  const Transform& actor_world,
                                  const Transform& previous_actor_world, f32 delta_seconds,
                                  const Activation& activation) noexcept;
    [[nodiscard]] Status deactivate() noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }

    /// Call before the fixed physics step; kinematic bones and powered motors follow this pose.
    [[nodiscard]] Status set_animation_target(Span<const Transform> model_pose,
                                              const Transform& actor_world,
                                              f32 delta_seconds) noexcept;
    /// Call after the fixed step. This advances per-body animation/physics blend weights.
    [[nodiscard]] Status advance_blend(f32 delta_seconds) noexcept;
    /// Read the blended model-space pose after a step; callers convert it to local space if needed.
    [[nodiscard]] Status sample_pose(Span<const Transform> animation_model,
                                     const Transform& actor_world,
                                     Span<Transform> out_model) const noexcept;
    /// A hit exposes this body's physics pose, then blends it back to its mode's base weight.
    [[nodiscard]] Status apply_hit(u16 joint, Vec3 impulse, f32 recovery_seconds) noexcept;

    [[nodiscard]] BodyHandle body(u16 joint) const noexcept;
    [[nodiscard]] f32 blend_weight(u16 joint) const noexcept;

private:
    struct Blend {
        f32 weight = 0.0f;
        f32 from = 0.0f;
        f32 target = 0.0f;
        f32 base = 0.0f;
        f32 elapsed = 0.0f;
        f32 duration = 0.0f;
    };

    [[nodiscard]] Status create_bodies(Span<const Transform> current,
                                       Span<const Transform> previous, const Transform& actor_world,
                                       const Transform& previous_actor_world,
                                       f32 delta_seconds) noexcept;
    [[nodiscard]] Status create_joints(Span<const Transform> current,
                                       const Transform& actor_world) noexcept;
    [[nodiscard]] Status validate_activation(Span<const Transform> current,
                                             Span<const Transform> previous, f32 delta_seconds,
                                             const Activation& activation) const noexcept;
    [[nodiscard]] Status move_kinematic(u16 joint, const Transform& world_pose,
                                        f32 delta_seconds) noexcept;
    [[nodiscard]] Status drive_joint(u16 joint, Span<const Transform> model_pose,
                                     const Transform& actor_world) noexcept;
    [[nodiscard]] Status initialize_motors(Span<const Transform> model_pose,
                                           const Transform& actor_world) noexcept;
    [[nodiscard]] bool dynamic_bone(u16 joint) const noexcept;
    static void start_blend(Blend& blend, f32 target, f32 seconds) noexcept;

    PhysicsServer& server_;
    WorldHandle world_;
    const animation::Skeleton& skeleton_;
    const Profile& profile_;
    Array<ShapeHandle> shapes_;
    Array<BodyHandle> bodies_;
    Array<ConstraintHandle> joints_;
    Array<Blend> blends_;
    Mode mode_ = Mode::Full;
    bool active_ = false;
};

}  // namespace cy::physics::ragdoll
