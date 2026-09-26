#pragma once
// Percentage-closer soft shadows for a directional light: the settings, the shape a frame's shader
// reads, and the C++ twin of `cy/shadow.slang`'s filter.
//
// `rendering-lighting-and-shadows` — "Shadow filtering": "a blocker search estimating average
// occluder depth, from which a penumbra radius scales the filter kernel, giving contact-hardening
// soft shadows". `virtual-shadows` — "Filtering and softness": softness "driven by physical source
// shape — angular radius for directional lights ... rather than an arbitrary per-light softness
// parameter", and "filtering quality SHALL be a budget lever".
//
// ================================================================================================
// THE ONE NUMBER THE LIGHT CONTRIBUTES
// ================================================================================================
//
// A directional light of angular radius `a` seen from a receiver `d` metres below a blocker casts a
// penumbra of radius `d * tan(a)` on that receiver. The shadow map stores light-space depth in
// [0, 1] across `depth_range` metres and covers `width` metres across its UV range, so the radius
// in UV for a depth difference `delta` is `delta * depth_range * tan(a) / width`. That product is
// `PcssShape::penumbra_per_depth`, and it is the whole of what the light's shape contributes: widen
// the sun and every shadow in the frame softens, with nothing else to tune. The engine's sun is
// 0.265 degrees across its radius — `kSunAngularRadius`.
//
// ================================================================================================
// WHAT IS HERE AND WHAT IS IN cy/shadow.slang
// ================================================================================================
//
// The shader holds the taps. This file holds the derivation of the shape (a constant per light per
// frame, which a shader should not recompute per pixel), the frame words it is uploaded as, and
// `pcss_visibility_reference` — the same three steps on the processor, expression for expression,
// which is what the host suite measures a penumbra with. A penumbra's width is a property of the
// arithmetic, so it is asserted numerically rather than read off a photograph.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/lighting/filtering.h>

namespace cy::rendering {

/// The sun's angular radius, in radians: half of the 0.53-degree disc.
inline constexpr f32 kSunAngularRadius = 0.004625F;

struct SoftShadowSettings {
    /// The light's angular radius in radians. Physical: the sun's by default.
    f32 angular_radius = kSunAngularRadius;
    /// Drives the tap counts through `shadow_sample_count` and `blocker_search_sample_count`, so
    /// the filter's cost is the same budget lever `ShadowLever::FilterQuality` moves.
    ShadowQuality quality = ShadowQuality::Ultra;
    /// The smallest kernel, in texels. About one, so a contact edge is filtered like PCF.
    f32 min_radius_texels = 1.0F;
    /// The largest kernel, in texels. It also bounds the blocker search.
    f32 max_radius_texels = 24.0F;
};

/// What the map covers: its light-space depth span and its width, both in metres, and its extent in
/// texels.
struct DirectionalShadowFootprint {
    f32 depth_range_metres = 0.0F;
    f32 width_metres = 0.0F;
    u32 extent = 0;
};

/// `cy/shadow.slang`'s `PcssShape`, field for field. Radii are in UV.
struct PcssShape {
    f32 penumbra_per_depth = 0.0F;
    f32 min_radius = 0.0F;
    f32 max_radius = 0.0F;
    u32 blocker_taps = 0;
    u32 filter_taps = 0;
};

/// The shape for one light and one map. A zero footprint gives a zero shape, which filters with a
/// zero-radius kernel — a hard shadow, never a garbage one.
[[nodiscard]] PcssShape make_pcss_shape(const SoftShadowSettings& settings,
                                        const DirectionalShadowFootprint& footprint) noexcept;

/// `cy/frame.slang`'s `softShadowControl` and `softShadowShape`: the flags, the contact term's
/// texture slot, the tap counts and the shape. `pipeline::kSoftShadowPcss` and
/// `kSoftShadowContact` are the flags.
void write_soft_shadow_words(u32 flags, u32 contact_slot, const PcssShape& shape, u32 control[4],
                             f32 shape_words[4]) noexcept;

/// `shadowDiscRotation`: interleaved gradient noise at a pixel centre, in radians.
[[nodiscard]] f32 shadow_disc_rotation(f32 pixel_x, f32 pixel_y) noexcept;

/// A square map of stored reversed-Z depths, row-major from the top-left, sampled at the nearest
/// texel with clamp-to-edge addressing.
struct ShadowDepthMap {
    Span<const f32> depths;
    u32 extent = 0;
    [[nodiscard]] f32 stored_depth(f32 u, f32 v) const noexcept;
};

/// What `pcssBlockerSearch` found.
struct PcssBlockers {
    f32 average_depth = 0.0F;
    f32 count = 0.0F;
};

[[nodiscard]] PcssBlockers pcss_blocker_search(const ShadowDepthMap& map, f32 u, f32 v,
                                               f32 receiver, f32 search_radius, f32 rotation,
                                               u32 taps) noexcept;

/// `pcssPenumbraRadius`: zero when nothing blocks.
[[nodiscard]] f32 pcss_directional_penumbra(f32 receiver, f32 average_blocker,
                                            f32 penumbra_per_depth) noexcept;

/// `pcssVisibility`, on the processor: the fraction of the filter's taps that see the light.
[[nodiscard]] f32 pcss_visibility_reference(const ShadowDepthMap& map, f32 u, f32 v, f32 receiver,
                                            const PcssShape& shape, f32 rotation) noexcept;

}  // namespace cy::rendering
