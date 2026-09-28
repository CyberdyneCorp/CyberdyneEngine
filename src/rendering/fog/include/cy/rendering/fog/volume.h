// SPDX-License-Identifier: MIT
#pragma once
// The froxel volume filled with the medium: the camera it is built for, the light it is lit by,
// the constants the device reads, the texture it is stored in, and the two host integrals the
// device is held to.
//
// `rendering-post-processing` — "Volumetric fog": "computed in a froxel volume — a camera-frustum
// -aligned 3D texture with exponential depth distribution — through: density injection (global
// fog plus fog volumes), lighting injection (per-froxel light evaluation with shadow sampling),
// filtering, and front-to-back scattering and transmittance integration."
//
// ================================================================================================
// ONE MARCH PER FROXEL COLUMN
// ================================================================================================
//
// The four steps the requirement names are one loop here, run once per (x, y) column of the volume,
// front to back:
//
//   for each slice, for each of `steps` equal sub-steps of it:
//       p          the sub-step's midpoint on the column's ray
//       medium     `sample_medium(p)`                                       density injection
//       J          sun_scattering * E_sun * V_shadow(p) + scattering * L_amb
//                  + emission                                               lighting injection
//       S, T       `integrate_froxel(J, sigma_t, ds, S, T)`                 integration
//   store (T, S) at the slice's far edge
//
// so the in-scattering and density are never stored at froxel resolution between passes, and the
// shadow map is read at the sub-steps rather than at the froxel centre alone — which is what
// resolves a shaft narrower than a slice is deep. THERE IS NO SEPARATE FILTER PASS: sampling at
// fixed sub-step midpoints is deterministic, so there is no noise for a filter or a history to
// remove. What it costs is aliasing of shafts thinner than a froxel, stated in the README.
//
// `integrate_froxel` is `rendering-post-processing`'s own step (`post/effects.h`): the analytic
// integral of a homogeneous sub-step, `S += T * J * (1 - exp(-sigma_t ds)) / sigma_t`, not a point
// sample times the length. `single_scattering_reference` below is the other integral — the
// single-scattering equation by brute-force quadrature, written independently — and the device's
// table is held to it.
//
// ================================================================================================
// THE CAMERA IS THE AERIAL PERSPECTIVE TABLE'S
// ================================================================================================
//
// `sky::AerialPerspectiveTable::View` and the table's header: a forward, right and up basis and the
// tangents of half the field of view, a column's ray `forward + right * ndc.x * tan_x + up * ndc.y
// * tan_y` with ndc at the column's centre and +y UP, and slice `i`'s far edge at
// `froxel_slice_depth(volume, i)` metres ALONG FORWARD, integrated from the eye. The fog header is
// that header word for word plus the eye, so one sampler shape reads both.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/fog/medium.h>
#include <cy/rendering/post/effects.h>
#include <cy/rendering/sky/tables.h>

namespace cy::rendering::fog {

/// The camera the volume is built for, in the frame's own camera-relative space.
struct FogView {
    /// Where every column's ray starts. Zero for the engine's frame, whose camera IS the origin of
    /// its relative space; a caller whose vertices are baked relative to some other point says so.
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    f32 tan_half_fov_x = 0.7364F;
    f32 tan_half_fov_y = 0.4142F;
    /// World position = relative position + this. The medium is authored in world metres; the
    /// froxels live in the frame's relative space.
    Vec3 world_offset{0.0F, 0.0F, 0.0F};
};

/// The basis out of a camera-relative-to-view matrix (right-handed, looking down -z) and a
/// projection's vertical field of view and aspect.
[[nodiscard]] FogView fog_view_from(const Mat4& relative_to_view, f32 fov_y_radians, f32 aspect,
                                    Vec3 eye, Vec3 world_offset) noexcept;

/// The light the medium is lit by, in the frame's own radiance units: whatever the surfaces around
/// the fog are shaded with, so the fog is lit consistently with them.
struct FogLight {
    /// Normalised, camera-relative, pointing FROM the medium TO the sun.
    Vec3 to_sun{0.0F, 1.0F, 0.0F};
    /// The sun's illuminance as it reaches the scene — what a surface's BRDF multiplies.
    Vec3 sun_illuminance{0.0F, 0.0F, 0.0F};
    /// The ambient radiance the frame's surfaces are lit by, taken as isotropic: the medium
    /// scatters `sigma_s * ambient` of it toward the eye.
    Vec3 ambient_radiance{0.0F, 0.0F, 0.0F};
};

/// The directional shadow map as the march reads it. Absent (`enabled` false), every point is lit.
struct FogShadow {
    bool enabled = false;
    /// Camera-relative position to (u, v, reference depth, w): the caller's own shadow projection
    /// with its own texture-coordinate convention folded in, so the march is told where a point
    /// lands in the map rather than which way the map's rows run. `shadow_uv_rows` builds it.
    Mat4 relative_to_uv = Mat4::identity();
    /// Added to the reference before comparing, in the map's depth units.
    f32 bias = 0.0F;
    /// Texels along one side of the square map.
    u32 extent = 1;
};

/// A reversed-Z shadow projection folded into texture coordinates: `u = ndc.x / 2 + 1/2` and
/// `v = ndc.y / 2 + 1/2`, or `1/2 - ndc.y / 2` with `flip_v` — the two conventions in this tree.
[[nodiscard]] Mat4 shadow_uv_rows(const Mat4& relative_to_shadow_clip, bool flip_v) noexcept;

/// How the volume is resolved.
struct FogSettings {
    FroxelVolume volume{};
    /// Sub-steps per slice. The shadow map and the medium are read at each one.
    u32 steps_per_slice = 2;
};

/// Float4 words of the device's constant block before the first fog volume, and per volume.
inline constexpr u32 kFogConstantHeaderWords = 17;
inline constexpr u32 kFogVolumeWords = 4;
inline constexpr u32 kFogConstantWords =
    kFogConstantHeaderWords + (kFogVolumeWords * kMaxFogVolumes);

/// The texels of the stored volume's first row that describe it, in order: forward and enabled,
/// right and tan x, up and tan y, the volume's shape, its planes and sub-steps, and the eye.
inline constexpr u32 kFogHeaderTexels = 6;

/// `volumetric_fog.slang`'s constant block, word for word. `pack_fog_constants` is its only writer.
struct FogConstants {
    Vec4 words[kFogConstantWords] = {};
};

/// The block for one frame. Refuses a volume too small to hold its header (fewer than three
/// columns), no slices, no sub-steps, or a non-positive depth range.
[[nodiscard]] Expected<FogConstants, Error> pack_fog_constants(const FogSettings& settings,
                                                               const FogView& view,
                                                               const FogLight& light,
                                                               const FogShadow& shadow,
                                                               const FogMedium& medium) noexcept;

// --- The stored volume --------------------------------------------------------------------------
//
// One `Rgba32Sfloat` texture, 2W by 1 + H D:
//
//   row 0                 the header, `kFogHeaderTexels` texels, the rest zero
//   row 1 + slice H + y   texel x:      transmittance.rgb, 1
//                         texel W + x:  in-scattering.rgb, 1
//
// with slice 0 nearest the eye and y counted UP the frame. A 2D texture rather than a 3D one
// because every consumer in the tree reads through a 2D table slot, and 2W by 1 + H D fits the
// default 160 x 90 x 64 volume in 320 by 5761.

struct FogTextureExtent {
    u32 width = 0;
    u32 height = 0;
};

[[nodiscard]] FogTextureExtent fog_texture_extent(const FroxelVolume& volume) noexcept;

// --- The atmosphere-table output ----------------------------------------------------------------
//
// The same march, stored in `sky::pack_aerial_perspective`'s layout — five header words, then
// (transmittance, 0) and (in-scattering, 0) per froxel, slice-major then row-major — so a consumer
// that applies aerial perspective through `cy/aerial_perspective.slang` applies the fog with it.
// With the atmosphere's own table bound, the air each slice spans is marched WITH the fog; see
// `integrate_fog_column`.

/// Float4 words of the table output for a volume.
[[nodiscard]] u64 fog_table_words(const FroxelVolume& volume) noexcept;

/// What the medium between the eye and a point does to that point's radiance: multiply by
/// `transmittance`, then add `in_scattering`.
struct FogAtPoint {
    Vec3 transmittance{1.0F, 1.0F, 1.0F};
    Vec3 in_scattering{0.0F, 0.0F, 0.0F};
};

/// `cy/volumetric_fog.slang`'s `cyFogAt` on the processor, over a read-back texture: the columns
/// interpolated bilinearly at their centres, the depth linearly between slice far edges from an
/// implicit identity slice at the eye. A point behind the eye, or a header whose `enabled` is off,
/// is returned untouched.
[[nodiscard]] FogAtPoint fog_at(Span<const Vec4> texels, FogTextureExtent extent,
                                Vec3 relative_position) noexcept;

// --- The two integrals --------------------------------------------------------------------------

/// The directional shadow map on the processor: `width = height = extent` depths, row-major,
/// reversed Z, read the way the march reads it.
struct HostShadowMap {
    Span<const f32> depth;
    u32 extent = 0;
};

/// The march's shadow lookup: 1 lit, 0 shadowed, a bilinear blend of four comparisons between.
/// Outside the map, or with the shadow off, 1.
[[nodiscard]] f32 fog_shadow_visibility(const FogShadow& shadow, const HostShadowMap& map,
                                        Vec3 relative_position) noexcept;

/// `volumetric_fog.slang`'s march over one column, transcribed: `transmittance` and
/// `in_scattering` receive one value per slice. `map` is read only when `shadow.enabled`.
///
/// With `air`, the atmosphere's froxel table built for the same camera, it is the table variant's
/// march: each slice's stretch of air is read out of the table — `T = T(far) / T(near)` and
/// `S = (S(far) - S(near)) / T(near)` — turned into the extinction and source of a homogeneous
/// medium that transmits and adds exactly that over the stretch, and marched with the fog, per
/// channel. An empty fog then reproduces the table at the slices' far edges, and an air table that
/// is off leaves the fog alone.
[[nodiscard]] Status integrate_fog_column(
    const FogSettings& settings, const FogView& view, const FogLight& light,
    const FogShadow& shadow, const HostShadowMap& map, const FogMedium& medium, u32 x, u32 y,
    Span<Vec3> transmittance, Span<Vec3> in_scattering,
    const sky::AerialPerspectiveTable* air = nullptr) noexcept;

/// The unit ray a column marches along, from the view's eye.
[[nodiscard]] Vec3 fog_column_direction(const FogSettings& settings, const FogView& view, u32 x,
                                        u32 y) noexcept;

/// The single-scattering equation along a ray, by midpoint quadrature over `steps` equal steps:
///
///     T(d) = exp(-integral sigma_t),
///     S(d) = integral T(s) [ sun_scattering(s) E V(s) + scattering(s) L_amb + emission(s) ] ds
///
/// with T(s) the transmittance from the eye to s taken at the step's midpoint. INDEPENDENT of the
/// march: no slices, no analytic sub-step, no shared code with `integrate_fog_column` but
/// `sample_medium` and `fog_shadow_visibility`, which are the medium and the map rather than the
/// integration. The march converges to this.
[[nodiscard]] FogAtPoint single_scattering_reference(const FogView& view, const FogLight& light,
                                                     const FogShadow& shadow,
                                                     const HostShadowMap& map,
                                                     const FogMedium& medium, Vec3 direction,
                                                     f32 distance, u32 steps) noexcept;

}  // namespace cy::rendering::fog
