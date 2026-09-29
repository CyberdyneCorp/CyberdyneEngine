// SPDX-License-Identifier: MIT
#pragma once
// Joints authored in the editor, read out of the world file. Issue #29, the physics tools.
//
// ================================================================================================
// THE COMPONENT, AND WHY ITS NAMES ARE A CONTRACT
// ================================================================================================
//
// `cy::physics::Joint` holds a `ConstraintDescription` whose endpoints are RUNTIME body handles,
// which no file can hold. The authored form is a `Joint` component on the node that owns body A,
// naming body B as an entity reference (or none, for the world), with the joint frame given as an
// anchor and an axis in body A's space. `editor/crates/cy-editor-services/src/joints.rs` writes
// exactly the names below; `a_joint_is_a_transaction.rs` there and `test_play.cpp` here hold the
// same golden world, so a spelling that drifted on either side is a failing test rather than a
// joint that silently does not exist.
//
// The anchor and the axis are in the body's rotated, UNSCALED frame, because a physics body has no
// scale. Frame B is not authored: `frame_on_target` derives it at play so that both anchors
// coincide in the authored pose, which is what an author placing two bodies and joining them means.

#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/constraints.h>

#include <string_view>

namespace cy::gameplay {

/// The component's name and its fields' names, as the editor writes them.
namespace joint_fields {
inline constexpr std::string_view kComponent = "Joint";
inline constexpr std::string_view kKind = "kind";
inline constexpr std::string_view kTarget = "target";
inline constexpr std::string_view kAnchor = "anchor";
inline constexpr std::string_view kAxis = "axis";
inline constexpr std::string_view kLimitMin = "limit_min";
inline constexpr std::string_view kLimitMax = "limit_max";
inline constexpr std::string_view kSwingY = "swing_y";
inline constexpr std::string_view kSwingZ = "swing_z";
inline constexpr std::string_view kLinearMin = "linear_min";
inline constexpr std::string_view kLinearMax = "linear_max";
inline constexpr std::string_view kAngularMin = "angular_min";
inline constexpr std::string_view kAngularMax = "angular_max";
inline constexpr std::string_view kMotorVelocity = "motor_velocity";
inline constexpr std::string_view kMotorMaxForce = "motor_max_force";
inline constexpr std::string_view kRatio = "ratio";
inline constexpr std::string_view kBreakForce = "break_force";
inline constexpr std::string_view kBreakTorque = "break_torque";
inline constexpr std::string_view kCollideConnected = "collide_connected";
}  // namespace joint_fields

/// One authored joint: its description with no bodies and frame B unset, and whom it joins.
struct AuthoredJoint {
    /// `body_a` and `body_b` are null, `frame_b` is identity: both are decided at play.
    physics::ConstraintDescription description;
    /// The editor identity of body B's node, or zero to join body A to the world.
    u64 target = 0;
};

/// The kind a `kind` word names — `cy::physics::constraint_type_name`'s spelling, so the ten words
/// are the engine's rather than a second list.
[[nodiscard]] Expected<physics::ConstraintType, Error> joint_kind_of(
    std::string_view word) noexcept;

/// The joint authored on `node`. `NotFound` when it carries none; `InvalidArgument`, naming the
/// word, when its kind is one this build does not know.
[[nodiscard]] Expected<AuthoredJoint, Error> authored_joint(
    const scene::serialization::World& world, const scene::serialization::WorldNode& node) noexcept;

/// Frame B, in body B's space, such that the two anchors coincide with the bodies where they are
/// now. `body_b` null is the world, whose frame is the world placement itself.
[[nodiscard]] Transform frame_on_target(const Transform& body_a, const Transform& frame_a,
                                        const Transform* body_b) noexcept;

/// The rotation that takes the frame's local X onto `axis`, which is what an authored axis means.
/// A zero axis is the identity.
[[nodiscard]] Quat joint_axis_rotation(Vec3 axis) noexcept;

}  // namespace cy::gameplay
