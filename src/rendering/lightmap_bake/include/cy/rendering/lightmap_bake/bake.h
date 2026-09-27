// SPDX-License-Identifier: MIT
#pragma once
// The lightmap bake: static lighting path traced per atlas texel, denoised, dilated and stitched,
// and the seeds for the dynamic caches from the same run.
//
// `rendering-global-illumination` — "Lightmap baking", "UV2 and chart packing" ("Seam artifacts").
//
// ================================================================================================
// THE PIPELINE, IN ORDER
// ================================================================================================
//
//   1. pack every receiving object into shared pages (`atlas.h`);
//   2. rasterise each object's UV2 triangles into its rectangle: a texel whose centre is inside a
//      triangle takes the interpolated world position and normal, and a texel the triangle only
//      grazes takes the nearest point of it, so the chart's border texels are real surface;
//   3. path trace each texel through `gi::PathTracer` over `MeshSceneTracer` — the same tracer the
//      real-time reference uses, over the level's triangles — for the configured bounces, and
//      reject a texel whose hemisphere mostly sees back faces, which is a texel buried inside a
//      neighbouring object;
//   4. denoise with `denoise::Denoiser`, the chart id as the hard boundary so no chart borrows
//      light from its neighbour in the atlas;
//   5. dilate each rectangle's covered texels outward over its padding and gutter;
//   6. reconcile seams: where two charts share a surface edge, move both sides' bilinear footprints
//      to their common mean so a seam is not visible at the bake resolution;
//   7. seed the surface cache, radiance cache and reflection probes from THE SAME path tracer.
//
// ================================================================================================
// WHAT IS STORED, AND IN WHICH UNITS
// ================================================================================================
//
// A texel stores the AMBIENT RADIANCE the frame multiplies a surface's albedo by — the mean incoming
// radiance over the cosine hemisphere, E / pi — which is what `cy/frame.slang`'s ambient term and
// its irradiance volume already hold. Three encodings, `LightmapMode`:
//
//   Irradiance    one RGBA texel: E(n) / pi about the geometric normal. The cheapest and blind to a
//                 normal map.
//   Directional   the same, plus the luminance-weighted mean incoming direction scaled by its
//                 directionality, `v`, and `w = 1 + v . n_geometric`. A shading normal n reads
//                 `rgb * max(0, 1 + v . n) / w`: exact at the geometric normal, and a normal map
//                 tilting toward the light brightens it. Two texels.
//   ShL1          the same fit per colour channel — `a_c + b_c . n` — so a coloured wall on one side
//                 of a surface tilts the colour as well as the brightness. Three texels.
//
// `gi::PathTracer` bounces with `albedo * irradiance`, the surface cache's convention; a Lambertian
// surface leaves `albedo * irradiance / pi`, which is the frame's. The bake hands the path tracer
// its lights scaled by 1/pi, which makes every bounce the frame's without changing the tracer.
//
// ================================================================================================
// WHAT IS NOT HERE
// ================================================================================================
//
// The shadow mask and light mobility (a stationary light's baked shadow term), incremental rebakes
// of one moved object's region, and a device bake. The next slice of #36 owns the first two; the
// CPU path tracer stays the reference either way.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/lightmap_bake/atlas.h>
#include <cy/rendering/lightmap_bake/scene.h>

namespace cy::rendering::lightmap_bake {

enum class LightmapMode : u8 {
    Irradiance = 0,
    Directional,
    ShL1,
    Count,
};

[[nodiscard]] const char* lightmap_mode_name(LightmapMode mode) noexcept;
/// RGBA texels per atlas texel: one, two or three planes.
[[nodiscard]] u32 lightmap_planes(LightmapMode mode) noexcept;

/// What a texel's light is made of.
enum class LightmapContent : u8 {
    /// Sky, emission and every bounce, and NOT the placed lights' direct term at the receiver: the
    /// frame computes that itself for lights it shades dynamically, which is every light until
    /// light mobility exists.
    Indirect = 0,
    /// The placed lights' shadowed direct term as well, for a level whose lights the frame does not
    /// shade.
    DirectAndIndirect,
};

struct LightmapBakeSettings {
    AtlasSettings atlas{};
    LightmapMode mode = LightmapMode::Directional;
    LightmapContent content = LightmapContent::Indirect;
    /// Path length, samples per texel, the sequence seed and the ray reach: `gi::BakeSettings`.
    gi::BakeSettings trace{};
    /// Off for reference comparison: the raw path-traced texels.
    bool denoise = true;
    /// Passes of border dilation. Zero derives it from the gutter and the widest padding.
    u32 dilation_passes = 0;
    bool reconcile_seams = true;
    /// Gauss-Seidel sweeps of the seam solve.
    u32 seam_iterations = 8;
    /// Spacing of the surface cards the path tracer resolves materials through, in metres.
    f32 surfel_spacing = 0.25F;
    /// A texel whose hemisphere rays meet back faces more often than this is inside geometry.
    f32 buried_threshold = 0.5F;
};

/// One page stack of RGBA planes, in the frame's texture layout: `width` texels by
/// `width * pages` rows, row-major from the top-left, plane after plane.
struct LightmapTexels {
    u32 width = 0;
    u32 height = 0;
    u32 planes = 0;
    /// `planes * width * height` RGBA texels.
    Array<Vec4> texels;

    [[nodiscard]] usize index(u32 plane, u32 x, u32 y) const noexcept {
        return (usize{plane} * width * height) + (usize{y} * width) + x;
    }
};

/// A baked level.
struct BakedLightmap {
    LightmapMode mode = LightmapMode::Directional;
    u32 page_size = 0;
    u32 pages = 0;
    u32 gutter_texels = 0;
    LightmapTexels texels;
    /// Per scene instance: its `gi_address`, or `kNoLightmapAddress` for one that receives none.
    Array<u32> addresses;
    /// Per atlas texel: whether the rasteriser covered it (1) or the dilation filled it (0).
    Array<u8> coverage;

    /// Bytes the texels occupy on the device as half floats.
    [[nodiscard]] u64 device_bytes() const noexcept {
        return u64{texels.width} * texels.height * texels.planes * 8U;
    }
};

struct LightmapBakeReport {
    u32 objects = 0;
    u32 pages = 0;
    u32 texels_covered = 0;
    u32 texels_buried = 0;
    u32 texels_dilated = 0;
    u32 seam_edges = 0;
    u32 seam_samples = 0;
    /// The largest bilinear disagreement across a seam before and after reconciliation, as a share
    /// of the mean covered texel's luminance.
    f32 seam_error_before = 0.0F;
    f32 seam_error_after = 0.0F;
    u64 rays = 0;
    f32 atlas_occupancy = 0.0F;
    /// The dynamic caches' seeds, from the same path tracer. Zero when no targets were given.
    gi::BakeReport seeds{};
};

/// The dynamic caches a bake seeds from the same run. Any may be null.
struct CacheSeedTargets {
    gi::SurfaceCache* surfaces = nullptr;
    gi::RadianceCache* radiance = nullptr;
    gi::ReflectionProbeSet* reflections = nullptr;
    /// The seeds' own sampling. Cheaper than the lightmap's: a seed only has to be plausible.
    gi::BakeSettings settings{};
};

/// Bake the level.
///
/// `seeds` may be null. When it names a surface cache and a radiance cache, both are seeded — and
/// the reflection probes when named — by the same `gi::PathTracer` the texels were traced with,
/// which is "the bake SHALL additionally produce seeds for the dynamic caches" from one run.
[[nodiscard]] Status bake_lightmaps(const LightmapScene& scene,
                                    const LightmapBakeSettings& settings,
                                    const CacheSeedTargets* seeds, BakedLightmap& out,
                                    LightmapBakeReport& report) noexcept;

/// The ambient radiance a lightmapped surface receives: the CPU reference `cy/frame.slang`'s
/// `lightmapAmbient` transcribes. Bilinear at `atlas_coordinate(address, uv2, ...)`, decoded by the
/// mode. `normal` is the SHADING normal, which a normal map moves.
[[nodiscard]] Vec3 sample_lightmap(const BakedLightmap& lightmap, u32 address, Vec2 uv2,
                                   Vec3 normal) noexcept;

/// One plane's RGBA, bilinear at an atlas coordinate in texels. Exposed for the seam cases.
[[nodiscard]] Vec4 sample_plane(const LightmapTexels& texels, u32 plane, Vec2 coordinate) noexcept;

/// The ground truth for a texel: what `LightmapContent` says it holds, path traced at `position`
/// about `normal` with `settings` over the same scene representation the bake uses.
///
/// Builds the tracer and the surface cards afresh, which is the point — the reference shares no
/// state with the bake that produced the atlas it checks.
[[nodiscard]] Status reference_ambient(const LightmapScene& scene,
                                       const LightmapBakeSettings& settings,
                                       Span<const Vec3> positions, Span<const Vec3> normals,
                                       Array<Vec3>& out) noexcept;

}  // namespace cy::rendering::lightmap_bake
