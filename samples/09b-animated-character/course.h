// SPDX-License-Identifier: MIT
#pragma once
// The course: a flight of stairs, a landing and a ramp down, walked by the character with its
// feet placed on them or not. Issue #92's artefact: `--course` in main.cpp.
//
// ================================================================================================
// WHAT IS REAL AND WHAT IS STAGED
// ================================================================================================
//
// REAL: the program is a `pose.clip` through a `pose.foot_ik` node, compiled by `compile_pose` and
// loaded like any cooked program; the animation system hands every solve the ground below and the
// entity's world placement; the legs, the hips and the feet are moved by `solve_foot_placement` in
// `Stage::Animation`, published into the pose world, and skinned on the device.
//
// STAGED, AND SAID SO: the ground is answered ANALYTICALLY by `CourseGround`, from the same numbers
// the boxes are drawn with, rather than by a physics world — the probe a game would hand the system
// is `PhysicsGroundProbe` (src/animation/ground/), and its own cases are in
// integration.animation_ground. The character's root is carried along the course at the walk's
// stance-foot speed, at the height a character controller's capsule would ride at — the ground
// averaged under it — because the Mixamo walk is an in-place take with no root motion to travel by.

#include <cy/animation/foot_placement.h>
#include <cy/animation/skeleton.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>

namespace cy::sample::character {

/// One box of the course, drawn with the frame's cube.
struct CourseBox {
    /// The cube's placement: rotation and scale applied to the unit cube, then the translation.
    Transform placement;
    /// Which of the stage's two ground materials it wears.
    u32 material = 0;
};

/// The ground of the course, from the same numbers the boxes are drawn with.
class CourseGround final : public animation::GroundProbe {
public:
    /// The height of the course at (x, z), and its normal there.
    [[nodiscard]] static f32 height(f32 x, f32 z, Vec3* normal = nullptr) noexcept;
    [[nodiscard]] animation::GroundHit cast_down(
        const animation::GroundQuery& query) const noexcept override;
};

/// The boxes the stage draws: the steps, the landing and the ramp.
void course_boxes(Array<CourseBox>& out) noexcept;

/// Where a character controller's capsule would carry the root at `z` along the course: the
/// ground averaged over the capsule's footprint, which rides a staircase as a ramp through the
/// steps rather than popping up each riser.
[[nodiscard]] f32 controller_height(f32 z) noexcept;

/// How far the course runs, along +z, from where the character starts.
inline constexpr f32 kCourseStart = -0.6F;
inline constexpr f32 kCourseEnd = 9.6F;

/// The cooked program the course plays: the walk, through foot placement, in one state. Foot
/// placement's weight is the parameter `feet`. The legs are found through the skeleton's humanoid
/// profile, so the graph names the character's own joints.
[[nodiscard]] Status cook_course_program(const animation::Skeleton& skeleton,
                                         const animation::SkeletonProfile& humanoid,
                                         Array<u8>& out) noexcept;

}  // namespace cy::sample::character
