#pragma once
// Directional shadow cascades: splits, stabilisation, and the blend between them. Task 4.4.2.
//
// `rendering-lighting-and-shadows` — "Directional shadow cascades": a configurable count (default
// 4), splits "derived from a blend of logarithmic and uniform distributions, with an
// artist-controllable blend factor", cascades "**stabilised**: the shadow projection SHALL be
// snapped to texel increments and sized by a bounding sphere of the cascade frustum, so shadows do
// not swim as the camera rotates", transitions blended over a band, and the depth range maximised
// by fitting the near plane to the caster bounds.
//
// ================================================================================================
// WHY A BOUNDING SPHERE AND NOT A BOUNDING BOX — THE ONE THING THAT MAKES SHADOWS STOP CRAWLING
// ================================================================================================
//
// A box fitted to a cascade's frustum corners changes SIZE as the camera rotates, because the
// frustum is not rotationally symmetric. Every size change rescales the shadow projection, every
// rescale moves every texel, and the result is shadow edges that crawl continuously while the
// camera turns — the artefact everybody recognises and nobody can localise.
//
// A sphere fitted to the same corners has a radius that depends only on the split distances and the
// field of view. It is bigger than the box, so it wastes some resolution; it does not change when
// the camera rotates, which is the entire point. With a fixed radius the projection's scale is
// constant, so a texel is a fixed world size, and the remaining motion — the sphere's centre
// sliding with the camera — is removed by snapping that centre to whole texels.
//
// Both halves are needed. A fixed size with an unsnapped centre still crawls, at a texel's scale.
//
// ================================================================================================
// NO DEVICE, NO GRAPH
// ================================================================================================
//
// Everything here is matrices and distances. The shadow passes that consume them are forward/'s,
// and keeping the arithmetic here means the swimming test — rotate the camera, assert the
// projection did not move by less than a texel — needs no GPU at all.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering {

inline constexpr u32 kMaxShadowCascades = 8;

struct CascadeSettings {
    /// `rendering-lighting-and-shadows`' default.
    u32 count = 4;
    /// Texels on a side, per cascade. What the texel snapping is quantised to.
    u32 resolution = 2048;
    /// 0 gives uniform splits, 1 gives logarithmic ones. The "artist-controllable blend factor";
    /// 0.75 is the usual compromise, favouring the logarithmic distribution that matches how
    /// perspective actually distributes detail.
    f32 split_blend = 0.75F;
    /// How far the shadowed range extends. Cascades cover [near, shadow_distance], not the whole
    /// view frustum: shadowing the far plane of an infinite projection is not a thing that can be
    /// done.
    f32 shadow_distance = 150.0F;
    /// The transition band between two cascades, as a fraction of the nearer one's extent. Both
    /// cascades are sampled inside it and blended.
    f32 transition_fraction = 0.1F;
    /// Where the last cascade starts fading out, as a fraction of `shadow_distance`. "WHEN a
    /// fragment is beyond the last cascade THEN shadowing SHALL fade out over a configurable
    /// distance rather than ending abruptly."
    f32 fade_start_fraction = 0.85F;
    /// How far behind the cascade's sphere the light's near plane is pulled, in units of the
    /// sphere's radius, so that casters outside the cascade still project into it. Larger keeps
    /// more casters and spends depth precision.
    f32 caster_extent_scale = 1.0F;
};

/// One cascade, fully derived.
struct ShadowCascade {
    /// The view-space depth range this cascade covers, in metres along the view direction.
    f32 near_distance = 0.0F;
    f32 far_distance = 0.0F;
    /// The stabilised bounding sphere, in world space.
    Vec3 center{0.0F, 0.0F, 0.0F};
    f32 radius = 0.0F;
    Mat4 view = Mat4::identity();
    /// Reversed-Z orthographic, like every other projection this engine builds.
    Mat4 projection = Mat4::identity();
    Mat4 view_projection = Mat4::identity();
    /// The world size of one shadow texel. What the normal-offset bias is scaled by, and the
    /// quantum the centre was snapped to.
    f32 texel_world_size = 0.0F;
};

/// The camera the cascades are fitted to. Semantic, for the same reason `render::Projection` is.
struct CascadeCamera {
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// Normalised, orthonormal. The camera's own basis: `forward` is its −Z.
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Vec3 right{1.0F, 0.0F, 0.0F};
    f32 fov_y_radians = 1.0471975512F;
    f32 aspect = 16.0F / 9.0F;
    f32 near_plane = 0.1F;
};

/// The split distances, blended between uniform and logarithmic.
///
/// `out` receives `settings.count` distances: the FAR distance of each cascade. The near distance
/// of cascade i is the far distance of cascade i−1, and of cascade 0 it is the camera's near plane.
[[nodiscard]] Status compute_cascade_splits(const CascadeSettings& settings, f32 near_plane,
                                            Span<f32> out) noexcept;

/// Fit, stabilise and snap every cascade. `out` must hold `settings.count` entries.
///
/// `light_direction` is the direction the light TRAVELS, normalised — the same convention
/// `GpuLight::direction` uses, so a caller cannot pass one and mean the other.
[[nodiscard]] Status compute_cascades(const CascadeSettings& settings, const CascadeCamera& camera,
                                      Vec3 light_direction, Span<ShadowCascade> out) noexcept;

/// Which cascade a view-space depth falls in, and how far through its transition band it is.
///
/// `blend` is 0 outside the band and rises to 1 at the far edge of it, where the NEXT cascade takes
/// over entirely. A shader samples both and lerps by it, which is what removes the seam.
struct CascadeLookup {
    u32 index = 0;
    f32 blend = 0.0F;
    /// 1 inside the shadowed range, falling to 0 past `fade_start_fraction · shadow_distance`.
    f32 fade = 1.0F;
};

[[nodiscard]] CascadeLookup select_cascade(const CascadeSettings& settings,
                                           Span<const ShadowCascade> cascades,
                                           f32 view_depth) noexcept;

}  // namespace cy::rendering
