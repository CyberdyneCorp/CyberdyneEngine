// SPDX-License-Identifier: MIT
#pragma once
// Motion blur: the settings, the constants the three dispatches read, and the CPU reference the
// suites compare against. `rendering-post-processing` — "Motion blur".
//
// ================================================================================================
// WHAT THE REQUIREMENT ASKS, AND WHERE EACH CLAUSE LIVES
// ================================================================================================
//
//   "use the velocity buffer"                the prepass velocity target, which since per-object
//                                            motion carries each instance's own movement
//   "compute per-tile maximum velocity"      `cyMotionBlurTileMax`: one tile of
//                                            `max_radius_pixels` square to its longest blur vector
//   "then gather along the dominant          `cyMotionBlurNeighbourMax` takes the longest over the
//    direction"                              3x3 tiles around each tile — the dominant direction a
//                                            pixel can be reached from — and `cyMotionBlurGather`
//                                            samples along it
//   "with depth-aware weighting so           the gather's weights: a sample NEARER than the pixel
//    background does not smear over          contributes as far as ITS OWN blur reaches, one
//    foreground"                             farther away only as far as the PIXEL's reaches
//   "parameterised by shutter angle, with    `MotionBlurSettings` below; the camera's share of a
//    separate per-object and camera blur     pixel's motion is reprojected from its depth, and the
//    scaling"                                remainder is the object's
//
// The filter is McGuire, Hennessy, Bukowski and Osman's reconstruction filter ("A Reconstruction
// Filter for Plausible Motion Blur", I3D 2012), whose three weights are written out in
// `motion_blur_reference_at` below and, expression for expression, in `motion_blur_gather.slang`.
//
// ================================================================================================
// THE ONE PHYSICAL NUMBER, AND WHERE IT COMES FROM
// ================================================================================================
//
// A shutter open for a fraction `f` of the frame interval integrates the image over `f` of the
// frame's motion: a surface moving `m` pixels a frame leaves a streak `f * m` long. The shutter
// ANGLE is that fraction in degrees, `360 * shutter_seconds / frame_seconds` — so the SAME shutter
// time that `CameraControls` exposes the picture with decides how long the streak is, and
// `settings_for_camera` is that sentence. 180 degrees is half a frame of motion, which is the
// requirement's own scenario; 0 degrees is no streak at all and is byte-identical to no stage.
//
// The streak is centred on the frame's instant (the shutter opens `f/2` of the motion before it
// and closes `f/2` after), so a pixel gathers `f * m / 2` to each side. That half-length is the
// BLUR VECTOR everything below stores, in pixels — McGuire's convention, in which the vector is a
// radius.
//
// ================================================================================================
// THE FREE PARAMETERS, EACH STATED WITH WHY IT EXISTS
// ================================================================================================
//
//   max_radius_pixels   The longest blur radius, which is also the tile edge: the neighbourhood of
//                       3x3 tiles reaches one tile each way, so a pixel can only be reached by a
//                       motion no longer than a tile. A cost bound, not a physical quantity. A
//                       surface that moves farther has its streak clamped to this and is otherwise
//                       drawn correctly.
//   samples             Gather taps per pixel. A quality bound: the streak is a line integral and
//                       this is how many points it is estimated at.
//   soft_depth_metres   Two samples closer in depth than this are blended as one surface rather
//                       than ordered. A tolerance for depth-buffer precision and for the thickness
//                       of the surface itself — the same role `ContactShadowSettings::thickness`
//                       plays, at the scale of the thinnest object that should still be ordered.
//
// `camera_scale` and `object_scale` are the requirement's "separate per-object and camera blur
// scaling". 1 and 1 is the physical camera; anything else is an artistic departure from it and is
// named as one.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::motion_blur {

struct MotionBlurSettings {
    /// The fraction of the frame interval the shutter is open, in degrees. See the header comment;
    /// `settings_for_camera` derives it from a shutter time.
    f32 shutter_angle_degrees = 180.0F;
    /// The camera's share of the motion, and every moving object's. 1 is physical.
    f32 camera_scale = 1.0F;
    f32 object_scale = 1.0F;
    /// The longest blur radius in pixels, and the tile edge. A cost bound; see the header comment.
    u32 max_radius_pixels = 24;
    /// Gather positions per pixel along the dominant direction, the middle one of which is the
    /// pixel itself and is skipped. Odd, so the taps are symmetric about the pixel.
    u32 samples = 15;
    /// Depth within which two samples are one surface, in metres. See the header comment.
    f32 soft_depth_metres = 0.05F;
};

/// The shutter angle a shutter time is at a frame rate: `360 * shutter / frame`, clamped to
/// [0, 360] — a shutter cannot stay open longer than the frame it exposes.
[[nodiscard]] f32 shutter_angle_for(f32 shutter_seconds, f32 frame_seconds) noexcept;

/// The physical camera: `settings` with its shutter angle taken from the shutter time that also
/// sets the exposure.
[[nodiscard]] MotionBlurSettings settings_for_camera(MotionBlurSettings settings,
                                                     f32 shutter_seconds,
                                                     f32 frame_seconds) noexcept;

/// What one frame's view contributes.
struct MotionBlurView {
    u32 width = 0;
    u32 height = 0;
    /// The UNJITTERED projection the depth was written with, reversed-Z. Its (2,2) and (3,2) terms
    /// turn a depth sample into metres.
    Mat4 projection = Mat4::identity();
    /// This frame's camera-relative to clip transform, UNJITTERED, and last frame's expressed about
    /// this frame's camera — the two matrices the prepass derives camera motion with. Only read
    /// when the camera and object scales differ: the camera's share of a pixel's motion is where
    /// last frame's camera saw the pixel's own depth.
    Mat4 relative_to_clip = Mat4::identity();
    Mat4 previous_relative_to_clip = Mat4::identity();
};

/// `CyMotionBlurConstants` in motion_blur_common.slang, word for word. The push-constant limit is
/// 128 bytes and this is exactly that.
struct alignas(16) MotionBlurConstants {
    /// This frame's clip space (unjittered NDC and depth) to last frame's clip space, as four rows.
    f32 current_to_previous[4][4] = {};
    /// xy: the extent in pixels, zw: its reciprocal.
    f32 extent[4] = {};
    /// x: the camera's blur per pixel of motion — shutter fraction times `camera_scale`, halved to
    /// a radius. y: the same for objects. z: the longest radius in pixels. w: the soft depth.
    f32 scales[4] = {};
    /// x: m22, y: m32 of the projection, for the view depth. z: 1 when the two scales differ and
    /// the camera's share must be separated out. w: unused.
    f32 depth[4] = {};
    /// x: gather samples, y: tile edge in pixels, z: tiles across, w: tiles down.
    u32 control[4] = {};
};
static_assert(sizeof(MotionBlurConstants) == 128, "CyMotionBlurConstants is eight float4");

/// The dispatches' constants. Refuses an empty extent, a zero radius, an even or zero sample count,
/// a negative scale and a non-invertible projection.
[[nodiscard]] Expected<MotionBlurConstants, Error> make_motion_blur_constants(
    const MotionBlurSettings& settings, const MotionBlurView& view) noexcept;

/// Tiles along an extent: the tile grid covers the image, so the last tile may be partial.
[[nodiscard]] u32 tile_count(u32 extent, u32 tile) noexcept;

/// Whether the constants blur anything at all. False at a closed shutter or with both scales at
/// zero: every blur vector is zero, every tile's maximum is zero, and the gather copies each pixel.
[[nodiscard]] bool blurs(const MotionBlurConstants& constants) noexcept;

// --- The CPU reference ---------------------------------------------------------------------

/// The inputs the dispatches read, as the device reads them: `width * height`, row-major from the
/// top-left.
struct MotionBlurInputs {
    u32 width = 0;
    u32 height = 0;
    /// Linear HDR colour. Alpha travels through untouched.
    Span<const Vec4> color;
    /// The prepass velocity target: normalised screen space, pointing AT THE PAST.
    Span<const Vec2> velocity;
    /// Reversed-Z depth in [0, 1]; 0 is the cleared far plane.
    Span<const f32> depth;
};

/// Where last frame's camera saw pixel (x, y) at reversed-Z `depth`, as a motion in pixels pointing
/// at the past — the prepass's convention, so it is the whole of a still surface's motion. What the
/// camera's share of a pixel's motion is separated out with.
[[nodiscard]] Vec2 camera_motion_pixels(const MotionBlurConstants& constants, u32 x, u32 y,
                                        f32 depth) noexcept;

/// A pixel's blur vector in pixels — the shutter-scaled half-motion, clamped to the longest radius.
[[nodiscard]] Vec2 blur_vector_at(const MotionBlurInputs& inputs,
                                  const MotionBlurConstants& constants, u32 x, u32 y) noexcept;

/// `cyMotionBlurTileMax` and `cyMotionBlurNeighbourMax` on the host: the longest blur vector of
/// each tile, then of each tile's 3x3 neighbourhood. `tiles` and `neighbours` are
/// `control.z * control.w` long.
[[nodiscard]] Status motion_blur_tiles_reference(const MotionBlurInputs& inputs,
                                                 const MotionBlurConstants& constants,
                                                 Span<Vec2> tiles, Span<Vec2> neighbours) noexcept;

/// `cyMotionBlurGather` at one pixel, on the host, given the neighbourhood maxima.
[[nodiscard]] Vec4 motion_blur_reference_at(const MotionBlurInputs& inputs,
                                            const MotionBlurConstants& constants,
                                            Span<const Vec2> neighbours, u32 x, u32 y) noexcept;

/// The whole image, through all three stages. `out` is `width * height`.
[[nodiscard]] Status motion_blur_reference(const MotionBlurInputs& inputs,
                                           const MotionBlurConstants& constants,
                                           Span<Vec4> out) noexcept;

}  // namespace cy::rendering::motion_blur
