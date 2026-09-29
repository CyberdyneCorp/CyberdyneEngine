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
// A texel stores the AMBIENT RADIANCE the frame multiplies a surface's albedo by — the mean
// incoming radiance over the cosine hemisphere, E / pi — which is what `cy/frame.slang`'s ambient
// term and its irradiance volume already hold. Three encodings, `LightmapMode`:
//
//   Irradiance    one RGBA texel: E(n) / pi about the geometric normal. The cheapest and blind to a
//                 normal map.
//   Directional   the same, plus `v`, the relative gradient of its luminance as the normal tilts
//                 (estimated from the same samples: see `trace.cpp`'s tilt response), and
//                 `w = 1 + v . n_geometric`. A shading normal n reads `rgb * max(0, 1 + v . n) /
//                 w`: exact at the geometric normal, right to first order around it, so a normal
//                 map tilting toward the light brightens it. Two texels.
//   ShL1          the same fit per colour channel — `a_c + b_c . n` — so a coloured wall on one
//   side
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
// A device bake: the CPU path tracer stays the reference. And the frame's USE of the shadow mask —
// the forward shader reading the mask plane for a stationary light's direct term — which is the
// device half of `add-lightmap-mobility-and-rebake` and not in this module.
//
// ================================================================================================
// LIGHT MOBILITY AND THE SHADOW MASK
// ================================================================================================
//
// Each `gi::GiLight` says what of it is baked (`gi::LightMobility`): a `Static` light's direct term
// and its bounce, a `Stationary` light's bounce and a SHADOW-MASK channel — the fraction of its
// extent each texel sees — and nothing of a `Movable` one. `Stationary` is the default and bakes
// exactly what every light baked before mobility existed. The mask is one RGBA plane beside the
// texels, a channel per stationary light, at most `kMaxShadowMaskLights` per level.
//
// ================================================================================================
// INCREMENTAL REBAKE
// ================================================================================================
//
// `rebake_lightmaps` re-solves the objects within an influence distance of a moved object's old
// and new placement and copies every other object's texels from the previous bake byte for byte.
// The object, not the texel, is the unit, because every post-process stage is chart-bounded.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/lightmap_bake/atlas.h>
#include <cy/rendering/lightmap_bake/mips.h>
#include <cy/rendering/lightmap_bake/scene.h>

#include <atomic>

namespace cy::rendering::lightmap_bake {

/// Stationary lights one level can give a shadow-mask channel: the plane's four channels.
inline constexpr u32 kMaxShadowMaskLights = 4;
/// Points over a light's extent each shadow-mask texel is tested against.
inline constexpr u32 kShadowMaskSamples = 64;

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
    /// Sky, emission and every bounce, and the direct term at the receiver of `Static` lights only:
    /// the frame computes the direct term itself for the lights it shades dynamically.
    Indirect = 0,
    /// The direct term of every light that is not `Movable` as well, for a level whose lights the
    /// frame does not shade. No shadow mask is written: no light is left for it to shadow.
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
    /// The denoiser's a-trous passes. Each doubles the reach; two reach three texels.
    u32 denoise_passes = 2;
    /// Passes of border dilation. Zero: until nothing more can be filled.
    u32 dilation_passes = 0;
    bool reconcile_seams = true;
    /// Conjugate-gradient iterations of the seam solve. A seam converges in about as many as its
    /// samples have distinct footprints; the solve stops early when it has.
    u32 seam_iterations = 32;
    /// Spacing of the surface cards the path tracer resolves materials through, in metres.
    f32 surfel_spacing = 0.25F;
    /// A texel whose hemisphere rays meet back faces more often than this is inside geometry.
    f32 buried_threshold = 0.5F;
    /// Refuse a level whose charts land closer together in their rectangle than bilinear filtering
    /// and the protected mip levels need, instead of only reporting them.
    bool refuse_short_padding = false;
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
    /// One RGBA plane in the texels' layout: channel c is the visible fraction of stationary light
    /// `shadow_lights[c]`, and 1 where no light has the channel. Empty when no light is stationary.
    LightmapTexels shadow_mask;
    /// The `gi::GiLight::id` of each channel's light, in channel order.
    Array<u64> shadow_lights;
    /// The `gi::GiLight::id` of every light whose DIRECT term at the receiver is in the texels:
    /// each `Static` light, and under `LightmapContent::DirectAndIndirect` each light that is not
    /// `Movable`. A lightmapped surface must not be shaded by these again, which is what
    /// `lightmaps::write_lightmaps` tells the frame.
    Array<u64> direct_lights;
    /// The mip levels below the base that the gutter and the chart padding protect
    /// (`AtlasLayout::mip_levels`), and the chain `build_lightmap_mips` filled for them: entry
    /// `level - 1` holds level `level`, `width >> level` by `height >> level`.
    u32 mip_levels = 0;
    LightmapTexels mip_texels[kMaxLightmapMipLevels];
    LightmapTexels mip_shadow_mask[kMaxLightmapMipLevels];

    /// Bytes the texels occupy on the device as half floats: every level of every plane.
    [[nodiscard]] u64 device_bytes() const noexcept {
        u64 bytes = u64{texels.width} * texels.height * texels.planes * 8U;
        for (u32 level = 0; level < mip_levels && level < kMaxLightmapMipLevels; ++level) {
            const LightmapTexels& mip = mip_texels[level];
            bytes += u64{mip.width} * mip.height * mip.planes * 8U;
        }
        return bytes;
    }
    /// Bytes the shadow mask occupies on the device, every level.
    [[nodiscard]] u64 shadow_mask_bytes() const noexcept {
        u64 bytes = u64{shadow_mask.width} * shadow_mask.height * shadow_mask.planes * 8U;
        for (u32 level = 0; level < mip_levels && level < kMaxLightmapMipLevels; ++level) {
            const LightmapTexels& mip = mip_shadow_mask[level];
            bytes += u64{mip.width} * mip.height * mip.planes * 8U;
        }
        return bytes;
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
    /// Scene instances whose charts land closer together in their rectangle than
    /// `required_chart_gap` empty texels, in instance order.
    Array<u32> padding_short;
    /// Empty texels two charts of one object need between them: two texels of the coarsest
    /// protected mip level.
    u32 required_chart_gap = 0;
    /// A rebake's own accounting. `incremental` is false for a full bake, and for a rebake that
    /// had to fall back to one (`fallback` says why).
    bool incremental = false;
    const char* fallback = "";
    /// Objects a rebake re-solved, and seams it left as the previous bake had them because only one
    /// side was re-solved.
    u32 objects_rebaked = 0;
    u32 boundary_seams = 0;
    /// True when `LightmapBakeProgress::cancel` stopped the bake.
    bool cancelled = false;
};

/// The stages a bake reports its progress through, in the order it runs them.
enum class LightmapBakeStage : u8 {
    /// Packing, rasterising and the chart-padding check.
    Prepare = 0,
    /// Path tracing every covered texel: the stage that is nearly all of a bake's time.
    Trace,
    /// Denoising, dilation and the seam solve.
    Filter,
    /// The mip chain and the dynamic caches' seeds.
    Finish,
    Count,
};

[[nodiscard]] const char* lightmap_bake_stage_name(LightmapBakeStage stage) noexcept;

/// A bake's progress and its cancellation, for a caller that runs it off the thread it draws on —
/// the editor's bake command.
///
/// NEITHER CHANGES WHAT IS BAKED: a bake that reports progress and is never cancelled writes the
/// bytes a bake with no `LightmapBakeProgress` writes.
struct LightmapBakeProgress {
    /// Called on the baking thread as work completes: `done` of `total` units of `stage`. The
    /// trace stage counts atlas texels, in steps of `kProgressTexels`; the others count one unit.
    void (*report)(void* user, LightmapBakeStage stage, u32 done, u32 total) noexcept = nullptr;
    void* user = nullptr;
    /// Read at every step: when it becomes true the bake stops, leaves its output unspecified, sets
    /// `LightmapBakeReport::cancelled` and fails with `ErrorCode::Unavailable`.
    const std::atomic<bool>* cancel = nullptr;

    [[nodiscard]] bool cancelled() const noexcept {
        return cancel != nullptr && cancel->load(std::memory_order_relaxed);
    }
    void step(LightmapBakeStage stage, u32 done, u32 total) const noexcept {
        if (report != nullptr) {
            report(user, stage, done, total);
        }
    }
};

/// Atlas texels between two progress reports, and two cancellation checks, of the trace stage.
inline constexpr u32 kProgressTexels = 1024;

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
///
/// `progress` may be null; see `LightmapBakeProgress`.
[[nodiscard]] Status bake_lightmaps(const LightmapScene& scene,
                                    const LightmapBakeSettings& settings,
                                    const CacheSeedTargets* seeds, BakedLightmap& out,
                                    LightmapBakeReport& report,
                                    const LightmapBakeProgress* progress = nullptr) noexcept;

/// What moved since `previous` was baked.
struct LightmapRebakeRequest {
    /// Indices into the scene's instances.
    Span<const u32> moved_instances;
    /// Each moved instance's world bounds BEFORE it moved, in the same order.
    Span<const Aabb> previous_bounds;
    /// How far a moved object's light reaches, in metres: objects with a surface texel within this
    /// of its old or new bounds are re-solved with it. The accuracy/cost trade, and the caller's.
    f32 influence_metres = 2.0F;
};

/// Re-solve what the move affects and copy the rest of `previous` byte for byte into `out`.
///
/// `scene` is the level AFTER the move, `settings` the ones `previous` was baked with. Falls back
/// to a full bake — and says so in `report.fallback` — when the level no longer packs to the same
/// rectangles or the mode or page layout changed.
[[nodiscard]] Status rebake_lightmaps(const LightmapScene& scene,
                                      const LightmapBakeSettings& settings,
                                      const BakedLightmap& previous,
                                      const LightmapRebakeRequest& request, BakedLightmap& out,
                                      LightmapBakeReport& report,
                                      const LightmapBakeProgress* progress = nullptr) noexcept;

/// A stationary light's shadow mask at an atlas coordinate, bilinear: 1 for a light with no
/// channel.
[[nodiscard]] f32 sample_shadow_mask(const BakedLightmap& lightmap, u64 light_id,
                                     Vec2 coordinate) noexcept;

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
