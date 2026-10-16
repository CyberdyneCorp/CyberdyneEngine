// SPDX-License-Identifier: MIT
#pragma once
// The screen-probe final gather: probes on a screen grid, traced, filtered over time and
// integrated per pixel. Issue #35, the follow-up of stage 4.
//
// `rendering-global-illumination` — "Screen-probe final gather".
//
// ================================================================================================
// WHY PROBES ON THE SCREEN, AND WHAT THE PER-PIXEL GATHER COST
// ================================================================================================
//
// `resolve_screen_pixel` (screen_resolve.h) spends, at EVERY pixel, a traced query of the radiance
// cache — one sphere trace to each contributing probe — and one final-gather ray. At 1920x1080 that
// was 11.6 ms on the RTX 5060 through the hardware tier, and most of it is work two neighbouring
// pixels of one surface repeat. Lumen's answer is the screen probe: a gather point every 8 or 16
// pixels that traces many rays, and every pixel between them integrating the probes that lie on its
// own surface. The rays a frame traces scale with the probes, not the pixels.
//
// A frame of it, on the resolve's grid (`ScreenGrid`: the frame's pixels at divisor one):
//
//   place       a UNIFORM probe at the centre pixel of every `spacing`-pixel tile, and an ADAPTIVE
//               probe at the centre of every half-tile cell the uniform probes cannot interpolate:
//               where the plane-distance and normal weights of the four uniform probes about it sum
//               below `adaptive_threshold` — a depth step, a crease, the sky's edge. Nowhere else.
//   reproject   each probe's previous radiance: of last frame's probes about the pixel its
//               position lands on (`temporal-rendering`'s motion), the one whose bilinear and
//               geometric weight is highest; none inside an invalidated region, as the per-pixel
//               history.
//   trace       `rays` rays a probe over its hemisphere through the world tier `screen_world_ray`
//               selects (ray queries where the device has them), each answered as the per-pixel ray
//               is — `screen_gather_sample` with the cache's answer at the probe as the stand-in —
//               from an origin moved clear of every other surface along the field's gradient.
//               Directions are drawn from a DEFENSIVE MIXTURE: a quarter uniform, a quarter cosine
//               and half from a 4x4 guide over the hemisphere's equal-area (cos theta, phi) square,
//               whose cells are weighted by the previous radiance (the reprojected probe) or, with
//               no history, by the radiance cache. Each ray is weighted by one over the mixture's
//               density, so the estimate is unbiased whatever the guide says, and the uniform
//               quarter bounds that weight at 8 pi.
//   store       the estimate projected onto second-order spherical harmonics, nine RGB
//               coefficients, and blended with the reprojected history by
//               max(1 / (count + 1), 1 / history_frames).
//   filter      under `ScreenProbeDenoiser::ProbeSpace` (the default), each probe's coefficients
//               averaged with the uniform probes' of the tiles about it — a tent over the tiles, a
//               world reach of `filter_reach` of its depth, and the plane and normal weight below
//               — into `filtered`, what the pixels read. The probe's own coefficients stay its
//               history, so the filter never compounds over frames. The pixels are then only
//               accumulated over time (`screen_signal_config`): the probes' denoiser is here.
//   integrate   every pixel: its irradiance from each probe's filtered coefficients about ITS OWN
//   normal,
//               weighted bilinearly over the uniform and adaptive grids and by plane distance and
//               normal agreement, combined with the far sky (and, with `cache_blend`, the cache's
//               answer interpolated from the probes) as `combine()` takes every source.
//
// The noisy signal it writes is the per-pixel resolve's, so the history and the publish downstream
// of it are unchanged; under `ScreenProbeDenoiser::Shared` so is the denoiser.
//
// ================================================================================================
// WHY SECOND-ORDER HARMONICS, AND NOT THE OCTAHEDRAL MAP LUMEN KEEPS
// ================================================================================================
//
// What a pixel reads from a probe is IRRADIANCE about its own normal, and irradiance is the
// radiance convolved with a clamped cosine, which attenuates band l by 1, 2/3 and 1/4 for l = 0, 1,
// 2 and by at most 1/24 beyond: nine coefficients hold 99 % of it for any lighting (Ramamoorthi and
// Hanrahan, 2001). They are 27 floats a probe where an 8x8 octahedral map is 192, and the
// integration evaluates them in closed form for each pixel's normal rather than filtering a map.
// Lumen keeps the map because it also reflects from it; here the rough specular reads the same
// coefficients with the cosine's attenuation scaled by the roughness, and a surface smooth enough
// for its own ray keeps one, as the per-pixel resolve does.
//
// ================================================================================================
// THIS FILE IS THE ORACLE OF A DEVICE PASS
// ================================================================================================
//
// `cy::rendering-gi-gpu`'s probe passes (gi_screen_probes.slang) transcribe this file expression
// for expression, and `render.gi_gpu_resolve` holds them to it. The randomness is `screen_hash` of
// the probe's id, so the host and the device draw the same rays. The one place a device may order
// work differently is the adaptive probes' slots, which it takes with an atomic: every probe is
// therefore addressed by its CELL through `adaptive_slot`, never by its slot.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/screen_inputs.h>

namespace cy::rendering::gi {

/// The second-order spherical harmonics a probe stores: nine coefficients per channel.
inline constexpr u32 kScreenProbeCoefficients = 9;
/// The guide's cells: a 4x4 grid over the hemisphere's (cos theta, phi) square, each 2 pi / 16 sr.
inline constexpr u32 kScreenProbeGuideEdge = 4;
inline constexpr u32 kScreenProbeGuideCells = kScreenProbeGuideEdge * kScreenProbeGuideEdge;
/// The most rays one probe traces: one per lane of the workgroup that traces it on a device.
inline constexpr u32 kScreenProbeMaxRays = 64;
/// No adaptive probe in a cell.
inline constexpr u32 kScreenProbeNone = 0xFFFFFFFFU;
/// The probe rays' hash stream, beside `kScreenStreamTrace` and its neighbours.
inline constexpr u32 kScreenStreamProbe = 8;
/// The stream that decides which probes trace a frame below a ray density of one.
inline constexpr u32 kScreenStreamProbeUpdate = 9;

/// The real spherical-harmonic basis to second order at a unit direction, in the usual order
/// (l, m) = (0,0), (1,-1), (1,0), (1,1), (2,-2), (2,-1), (2,0), (2,1), (2,2).
void screen_probe_basis(Vec3 direction, f32 out[kScreenProbeCoefficients]) noexcept;

/// Evaluate nine RGB coefficients at `direction`, each band scaled by `band[l]`: (1, 1, 1) is the
/// radiance, (1, 2/3, 1/4) the mean cosine-weighted radiance about `direction` — what the caches
/// call irradiance (`decode_payload`'s units). Never negative.
[[nodiscard]] Vec3 screen_probe_evaluate(const Vec3 coefficients[kScreenProbeCoefficients],
                                         Vec3 direction, const f32 band[3]) noexcept;

/// The grid of probes over a resolve grid of `width` x `height` cells, `spacing` cells apart.
struct ScreenProbeLayout {
    u32 width = 0;
    u32 height = 0;
    /// Cells between uniform probes; the adaptive cells are half of it.
    u32 spacing = 0;
    u32 tiles_x = 0;
    u32 tiles_y = 0;
    u32 cell = 0;
    u32 cells_x = 0;
    u32 cells_y = 0;
    /// The most adaptive probes a frame keeps: as many as there are uniform ones.
    u32 capacity = 0;

    [[nodiscard]] u32 uniform_count() const noexcept { return tiles_x * tiles_y; }
    [[nodiscard]] u32 cell_count() const noexcept { return cells_x * cells_y; }
    /// Probe records a frame holds: the uniform ones, then the adaptive slots.
    [[nodiscard]] u32 records() const noexcept { return uniform_count() + capacity; }
    /// The grid cell a uniform probe stands on: its tile's centre, clamped to the grid.
    [[nodiscard]] u32 uniform_pixel(u32 tile) const noexcept;
    /// The grid cell an adaptive probe stands on: its half-tile cell's centre, clamped.
    [[nodiscard]] u32 adaptive_pixel(u32 cell_index) const noexcept;
};

/// The layout for a grid of `width` x `height` at `spacing` cells. Zero spacing is no layout.
[[nodiscard]] ScreenProbeLayout screen_probe_layout(u32 width, u32 height, u32 spacing) noexcept;

/// The rays a probe traces when it traces: `rays`, at least one and never above
/// `kScreenProbeMaxRays`.
[[nodiscard]] u32 screen_probe_rays(const ScreenResolveSettings& settings) noexcept;

/// Whether probe `id` traces its rays in `frame` at the resolve's `ray_density`: every probe at a
/// density of one, that fraction of them below — a hash of the probe and the frame, as
/// `screen_traces` draws a pixel. A probe that does not trace keeps the probe it stands for.
[[nodiscard]] bool screen_probe_traces(u32 id, u64 frame, f32 density) noexcept;

/// The spacing in grid cells for `spacing_pixels` frame pixels at the resolve grid's `divisor`:
/// never below two, so a half-tile cell is at least one cell.
[[nodiscard]] u32 screen_probe_grid_spacing(u32 spacing_pixels, u32 divisor) noexcept;

/// How much a probe at `probe_position` facing `probe_normal` stands for a surface at `position`
/// facing `normal`, `depth` metres from the eye: its distance from the probe's plane against
/// `plane_tolerance * depth`, falling linearly to zero, times the normals' cosine to the eighth.
[[nodiscard]] f32 screen_probe_geometry_weight(Vec3 probe_position, Vec3 probe_normal,
                                               Vec3 position, Vec3 normal, f32 depth,
                                               f32 plane_tolerance) noexcept;

/// One probe of a frame.
struct ScreenProbe {
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// The view depth of its surface. Zero is no probe: a tile whose centre is sky.
    f32 depth = 0.0F;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// The hash key: a uniform probe's tile, or the uniform count plus an adaptive probe's cell.
    u32 id = 0;
    /// The radiance, after the temporal blend: what the next frame reprojects.
    Vec3 coefficients[kScreenProbeCoefficients] = {};
    /// Frames of history the coefficients carry, at most `history_frames`.
    f32 history = 0.0F;
    /// What the pixels integrate: the coefficients, or under `ScreenProbeDenoiser::ProbeSpace` the
    /// coefficients averaged with the neighbours' on the probe's own surface (`filter`). Never
    /// reprojected, so the filter does not compound frame over frame.
    Vec3 filtered[kScreenProbeCoefficients] = {};
    /// The radiance cache's diffuse answer at the probe, and its confidence: each ray's stand-in,
    /// and the cache's source in the combine when `cache_blend` is on.
    Vec3 cache{0.0F, 0.0F, 0.0F};
    f32 cache_confidence = 0.0F;
    /// One of the field's sky-visibility rays from the probe, in the far field; one elsewhere.
    f32 openness = 1.0F;
};

/// What a frame of probes did, for a test and for the profile.
struct ScreenProbeStats {
    u32 uniform = 0;
    u32 adaptive = 0;
    /// Adaptive candidates refused because `capacity` was spent.
    u32 refused = 0;
    u32 rays = 0;
    /// Probes that found a history.
    u32 reprojected = 0;
    /// Probes that kept the history they found rather than tracing, below a ray density of one.
    u32 carried = 0;
};

/// The probes of one view, and of the frame before it. `ScreenResolve::produce` runs it when the
/// settings ask for `GiGather::ScreenProbes`; a test may drive the stages one by one.
class ScreenProbeGather {
public:
    ScreenProbeGather() noexcept = default;

    /// Size for a resolve grid of `width` x `height` at `spacing` grid cells. A new layout forgets
    /// the history; the same one keeps it.
    [[nodiscard]] Status resize(u32 width, u32 height, u32 spacing) noexcept;
    /// Forget the previous frame's probes: a cut, a teleport.
    void reset_history() noexcept { reset_ = true; }

    /// This frame's probes on `surfaces`, the resolve grid: the previous frame's become the
    /// history, and the new set is placed — uniform, then adaptive in cell order.
    [[nodiscard]] Status place(const ScreenSurfaces& surfaces,
                               const ScreenProbeSettings& settings) noexcept;
    /// Reproject, trace and blend every placed probe.
    void trace(const ScreenSurfaces& surfaces, const ScreenResolveSettings& settings,
               const ScreenResolveSources& sources) noexcept;
    /// The probe-space filter, after `trace`: each placed probe's `filtered` coefficients, its own
    /// and the uniform probes' of the tiles about it within `filter_tiles`, weighted by a tent over
    /// the tiles and by how well each neighbour stands for the probe's surface
    /// (`screen_probe_geometry_weight`). Under `ScreenProbeDenoiser::Shared`, or with no reach,
    /// `filtered` stays the coefficients.
    void filter(const ScreenProbeSettings& settings) noexcept;
    /// One pixel's two signals from the probes. `view_direction` is from the eye to the surface.
    [[nodiscard]] ScreenPixel integrate(u32 pixel, Vec3 position, Vec3 normal, f32 roughness,
                                        f32 depth, f32 camera_distance, Vec3 view_direction,
                                        const ScreenResolveSettings& settings,
                                        const ScreenResolveSources& sources) const noexcept;

    [[nodiscard]] const ScreenProbeLayout& layout() const noexcept { return layout_; }
    /// This frame's records: `layout().uniform_count()` uniform probes, then the adaptive slots.
    [[nodiscard]] Span<const ScreenProbe> probes() const noexcept { return probes_[slot_].span(); }
    /// The adaptive slot of each half-tile cell this frame, or `kScreenProbeNone`.
    [[nodiscard]] Span<const u32> adaptive_slots() const noexcept { return slots_[slot_].span(); }
    /// This frame's probe in an adaptive cell, or null.
    [[nodiscard]] const ScreenProbe* adaptive(u32 cell_index) const noexcept;
    [[nodiscard]] const ScreenProbeStats& stats() const noexcept { return stats_; }

private:
    /// The coefficients and history count the previous frame holds for a probe, by the same
    /// weights the integration uses about the pixel the probe's position lands on.
    /// `matched` is that previous probe.
    [[nodiscard]] bool reproject(const ScreenProbe& probe, const ScreenResolveSources& sources,
                                 const ScreenProbeSettings& settings,
                                 Vec3 coefficients[kScreenProbeCoefficients], f32& history,
                                 const ScreenProbe*& matched) const noexcept;
    void trace_probe(ScreenProbe& probe, const ScreenResolveSettings& settings,
                     const ScreenResolveSources& sources) noexcept;

    ScreenProbeLayout layout_{};
    Vec3 camera_{0.0F, 0.0F, 0.0F};
    Array<ScreenProbe> probes_[2];
    Array<u32> slots_[2];
    u32 adaptive_count_[2] = {0, 0};
    u32 slot_ = 0;
    bool previous_valid_ = false;
    bool placed_ = false;
    bool reset_ = false;
    ScreenProbeStats stats_{};
};

}  // namespace cy::rendering::gi
