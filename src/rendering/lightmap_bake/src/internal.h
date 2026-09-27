// SPDX-License-Identifier: MIT
#pragma once
// The bake's stages, shared between its translation units and by nothing outside them.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/lightmap_bake/bake.h>

namespace cy::rendering::lightmap_bake::detail {

/// Nothing owns this texel.
inline constexpr u32 kNoOwner = ~0U;

/// What rasterising a texel found.
enum class TexelState : u8 {
    /// Outside every triangle; the dilation fills it if it is inside a rectangle.
    Empty = 0,
    /// A triangle's surface, sampled at the texel centre or at the nearest point of a triangle the
    /// texel grazes.
    Surface,
    /// Traced, and found to be inside geometry. The dilation fills it like an empty texel.
    Buried,
};

/// One atlas texel's rasterised surface.
struct TexelSurface {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// The scene instance whose rectangle the texel is in, or `kNoOwner`.
    u32 owner = kNoOwner;
    /// Globally unique per chart; `kNoOwner` where there is no surface.
    u32 chart = kNoOwner;
    TexelState state = TexelState::Empty;
    /// True for a texel taken from the nearest point of a triangle rather than its centre.
    bool conservative = false;
    /// The squared UV distance a conservative texel was taken at, so the nearest triangle wins.
    f32 distance = 0.0F;
};

/// A texel's radiance moments, in the units the planes are encoded from. Linear in the samples,
/// which is what lets the denoiser, the dilation and the seam solve work on them directly.
struct TexelMoments {
    /// The mean incoming radiance over the cosine hemisphere: E / pi.
    Vec3 mean{0.0F, 0.0F, 0.0F};
    /// The mean of luminance times direction.
    Vec3 luminance_direction{0.0F, 0.0F, 0.0F};
    /// The mean of each channel times direction.
    Vec3 channel_direction[3] = {};
    /// The mean luminance.
    f32 luminance = 0.0F;
};

/// The atlas as the stages see it: `width` by `height` texels, every page stacked.
struct Canvas {
    u32 width = 0;
    u32 height = 0;
    Array<TexelSurface> surfaces;
    Array<TexelMoments> moments;

    [[nodiscard]] usize index(u32 x, u32 y) const noexcept { return (usize{y} * width) + x; }
};

/// One seam: an edge two charts of one object share, as atlas coordinates on each side.
struct SeamEdge {
    Vec2 a0{};
    Vec2 a1{};
    Vec2 b0{};
    Vec2 b1{};
};

[[nodiscard]] f32 luminance(Vec3 value) noexcept;

/// Rasterise every receiving instance into its rectangle, and collect its seams.
[[nodiscard]] Status rasterise(const LightmapScene& scene, const BakedLightmap& lightmap,
                               Canvas& canvas, Array<SeamEdge>& seams) noexcept;

/// The texel's moments, path traced. `sequence` seeds the draw.
struct TraceContext {
    const gi::PathTracer* path = nullptr;
    const MeshSceneTracer* tracer = nullptr;
    /// The lights the path tracer was built with, already scaled by 1/pi.
    Span<const gi::GiLight> lights;
    const LightmapBakeSettings* settings = nullptr;
};

[[nodiscard]] TexelMoments trace_moments(const TraceContext& context, Vec3 position, Vec3 normal,
                                         u32 sequence, u32 samples) noexcept;
/// Whether the hemisphere about `normal` mostly meets back faces.
[[nodiscard]] bool buried(const TraceContext& context, Vec3 position, Vec3 normal,
                          u32 sequence) noexcept;

/// Surface cards for the GI scene the path tracer resolves materials through.
[[nodiscard]] Status build_surfels(const LightmapScene& scene, const MeshSceneTracer& tracer,
                                   f32 spacing, gi::GiScene& out) noexcept;

[[nodiscard]] Status denoise_moments(Canvas& canvas, LightmapMode mode, u32 samples) noexcept;
/// Returns how many texels it filled.
[[nodiscard]] u32 dilate(Canvas& canvas, u32 passes) noexcept;
/// Reconcile the seams; returns the samples it solved over, and the errors before and after.
[[nodiscard]] u32 reconcile_seams(Canvas& canvas, Span<const SeamEdge> seams, u32 iterations,
                                  bool apply, f32& error_before, f32& error_after) noexcept;

/// The moments' planes in the mode's encoding, rounded through half floats.
void encode_planes(const Canvas& canvas, LightmapMode mode, LightmapTexels& texels) noexcept;

}  // namespace cy::rendering::lightmap_bake::detail
