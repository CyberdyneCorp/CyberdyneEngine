// SPDX-License-Identifier: MIT
#pragma once
// The scene the collision suites collide with: a flight of stairs on a floor, as an exact signed
// distance field, and the depth buffer a camera would see of it. `integration.vfx`,
// `render.vfx_gpu`.
//
// EXACT, AND THAT IS THE POINT OF IT. The CPU suite asks whether a particle rests ON a step, and a
// field that was itself an approximation would make every bound in that suite a statement about
// the approximation. The device suite places the same boxes in the GI field and compares the two
// paths through it instead — see test_vfx_collision_gpu.cpp.

#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/vfx/collision.h>

#include <vector>

namespace cy::vfx_test {

/// The stairs: `kSteps` steps climbing along +z from the origin, each `kTread` deep and `kRise`
/// high and `kStairWidth` wide in x, standing on the floor `y = 0`.
inline constexpr u32 kSteps = 5;
inline constexpr f32 kTread = 0.4F;
inline constexpr f32 kRise = 0.25F;
inline constexpr f32 kStairWidth = 3.0F;

/// Step `index`'s centre and half extents, as an axis-aligned box.
void step_box(u32 index, Vec3& centre, Vec3& half_extents) noexcept;

/// The exact distance to an axis-aligned box.
[[nodiscard]] f32 box_distance(Vec3 point, Vec3 centre, Vec3 half_extents) noexcept;

/// The stairs and the floor as a `CollisionField`. Counts its samples, so the budget's collision
/// lever is a number a case can read.
class StairsField final : public vfx::CollisionField {
public:
    explicit StairsField(bool floor = true) noexcept : floor_(floor) {}

    [[nodiscard]] f32 distance(Vec3 point) const noexcept override;

    [[nodiscard]] u64 samples() const noexcept { return samples_; }
    void reset_samples() noexcept { samples_ = 0; }

private:
    bool floor_ = true;
    mutable u64 samples_ = 0;
};

/// A camera's depth buffer of a field, ray marched, and the view a contact reads it through.
struct DepthScene {
    std::vector<f32> depths;
    vfx::DepthCollisionView view;
};

/// Render `field` from `eye` toward `target` into a `width` x `height` reversed-Z depth buffer.
[[nodiscard]] DepthScene render_depth(const vfx::CollisionField& field, Vec3 eye, Vec3 target,
                                      u32 width, u32 height, f32 fov_y = 1.0F) noexcept;

/// The floor alone, `y = 0`, intersected rather than marched: a depth buffer of any size in the
/// time a frame takes to fill one, for a case that measures rather than compares.
[[nodiscard]] DepthScene render_floor_depth(Vec3 eye, Vec3 target, u32 width, u32 height,
                                            f32 fov_y = 1.0F) noexcept;

}  // namespace cy::vfx_test
