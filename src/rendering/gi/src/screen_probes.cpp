// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/screen_probes.h>

#include <cy/core/math/scalar.h>
#include <cy/rendering/gi/screen_resolve.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::rendering::gi {
namespace {

/// The irradiance bands: the clamped cosine's attenuation of l = 0, 1, 2, divided by pi.
constexpr f32 kIrradianceBands[3] = {1.0F, 2.0F / 3.0F, 0.25F};
constexpr f32 kRadianceBands[3] = {1.0F, 1.0F, 1.0F};
/// Which band each coefficient is in.
constexpr u32 kBandOf[kScreenProbeCoefficients] = {0, 1, 1, 1, 2, 2, 2, 2, 2};
/// The guide's cells, each 2 pi / 16 of the hemisphere: the density of a direction drawn from one.
constexpr f32 kGuideCellDensity = static_cast<f32>(kScreenProbeGuideCells) / math::kTwoPi;
/// The most of a probe's rays the guide draws. A guide cell of zero weight — where the previous
/// radiance's harmonics dip below zero at the horizon — is never drawn, so a guide alone is biased
/// there (2.8 % dark under an open sky); a tenth drawn uniform and cosine keeps every direction
/// drawable, and the estimate unbiased.
constexpr f32 kMaxGuidedShare = 0.9F;
/// The most steps a probe's ray origin takes out from the surfaces about it.
constexpr u32 kOriginSteps = 4;
/// Below this an interpolation found nothing to interpolate.
constexpr f32 kWeightFloor = 1.0e-4F;
/// A previous probe stands for this one when its bilinear and geometric weight together reach this:
/// a quarter, which the nearest of four always has on its own surface.
constexpr f32 kHistoryMatch = 0.25F;

[[nodiscard]] f32 luminance_of(Vec3 c) noexcept {
    return (0.2126F * c.x) + (0.7152F * c.y) + (0.0722F * c.z);
}

/// Up to four taps of a bilinear interpolation over a lattice of `count_x` x `count_y` points,
/// point i standing at `i * step + half` (clamped to the grid): the cell (x, y) between them.
struct Taps {
    u32 index[4] = {0, 0, 0, 0};
    f32 weight[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u32 count = 0;
};

void axis_taps(u32 coordinate, u32 step, u32 count, u32& base, f32& fraction) noexcept {
    const u32 half = step / 2U;
    const f32 g = (static_cast<f32>(coordinate) - static_cast<f32>(half)) / static_cast<f32>(step);
    if (g <= 0.0F) {
        base = 0;
        fraction = 0.0F;
        return;
    }
    base = static_cast<u32>(g);
    fraction = g - static_cast<f32>(base);
    if (base + 1U >= count) {
        base = count - 1U;
        fraction = 0.0F;
    }
}

[[nodiscard]] Taps bilinear_taps(u32 x, u32 y, u32 step, u32 count_x, u32 count_y) noexcept {
    Taps taps;
    u32 bx = 0;
    u32 by = 0;
    f32 fx = 0.0F;
    f32 fy = 0.0F;
    axis_taps(x, step, count_x, bx, fx);
    axis_taps(y, step, count_y, by, fy);
    for (u32 j = 0; j < 2; ++j) {
        for (u32 i = 0; i < 2; ++i) {
            const f32 weight = (i == 0 ? 1.0F - fx : fx) * (j == 0 ? 1.0F - fy : fy);
            if (weight <= 0.0F) {
                continue;
            }
            taps.index[taps.count] = ((by + j) * count_x) + (bx + i);
            taps.weight[taps.count] = weight;
            taps.count += 1;
        }
    }
    return taps;
}

/// A direction on the hemisphere about `normal` from its (cos theta, phi / 2 pi) square.
[[nodiscard]] Vec3 hemisphere_direction(Vec3 normal, f32 z, f32 turn) noexcept {
    Vec3 tangent;
    Vec3 bitangent;
    screen_basis(normal, tangent, bitangent);
    const f32 radius = std::sqrt(math::max(0.0F, 1.0F - (z * z)));
    const f32 angle = math::kTwoPi * turn;
    return normalized_or((tangent * (radius * std::cos(angle))) +
                             (bitangent * (radius * std::sin(angle))) + (normal * z),
                         normal);
}

[[nodiscard]] u32 guide_cell(f32 z, f32 turn) noexcept {
    const u32 i = std::min(static_cast<u32>(z * static_cast<f32>(kScreenProbeGuideEdge)),
                           kScreenProbeGuideEdge - 1U);
    const u32 j = std::min(static_cast<u32>(turn * static_cast<f32>(kScreenProbeGuideEdge)),
                           kScreenProbeGuideEdge - 1U);
    return (i * kScreenProbeGuideEdge) + j;
}

/// The world tier a probe's rays go through, as `screen_world_ray` selects it.
[[nodiscard]] RadianceSource tier_source(const ScreenResolveSources& sources) noexcept {
    return sources.hardware != nullptr && sources.hardware->available()
               ? RadianceSource::HardwareTrace
               : RadianceSource::SoftwareTrace;
}

/// The radiance cache's diffuse answer at a probe: its rays' stand-in.
[[nodiscard]] RadianceSample cache_at(const ScreenResolveSources& sources, Vec3 position,
                                      Vec3 normal) noexcept {
    if (sources.cache == nullptr) {
        return RadianceSample{};
    }
    if (sources.field != nullptr) {
        return sources.cache
            ->sample_traced(position, normal, normal, sources.cache_frame, *sources.field)
            .diffuse;
    }
    return sources.cache->sample(position, normal, sources.cache_frame);
}

/// WHERE A PROBE'S RAYS LEAVE FROM: its surface lifted along its normal — by `lift` on the hardware
/// tier, by at least one finest voxel on the software tier, as every final-gather ray is lifted
/// (`screen_world_ray`) — AND THEN MOVED OUT, along the field's gradient, to that same clearance
/// from any OTHER surface.
///
/// A probe stands on a pixel, and a pixel can be anywhere on its surface: two centimetres from a
/// 10 cm wall's foot, or on the very edge where a wall meets the ceiling. Lifted along its own
/// normal alone, the first is still inside the software trace's hit threshold of the wall, and its
/// first step lands INSIDE the wall, where the gradient points anywhere and the hit's card lookup
/// reads the far side's card; the second is still ON the ceiling's plane, and a ray up leaves the
/// room through it to the sky. A per-pixel ray from such a pixel is one sample weighed against the
/// cache; a probe's estimate is the mean of its rays, and it often has no cache to stand in. In the
/// sealed room behind the 10 cm wall at 1080p the worst pixel took a quarter of the lit room's
/// light through the software tier, and 5 % through the hardware tier from the corners. Moved out,
/// the first meets the wall's near face from outside and the second the ceiling from inside. The
/// move is DDGI's probe relocation, for one ray origin; a probe where a side wall, the floor and
/// the 10 cm wall meet needs more than one step of it.
[[nodiscard]] Vec3 probe_origin(const ScreenResolveSources& sources, const ScreenProbe& probe,
                                f32 lift) noexcept {
    const bool hardware = sources.hardware != nullptr && sources.hardware->available();
    if (sources.field == nullptr) {
        return probe.position + (probe.normal * lift);
    }
    const DistanceField& field = *sources.field;
    const f32 voxel = field.voxel_size();
    const f32 clearance = hardware ? lift : std::max(lift, voxel);
    Vec3 origin = probe.position + (probe.normal * clearance);
    if (voxel <= 0.0F) {
        return origin;
    }
    // A step at a time, because in a corner of three surfaces one step along the gradient clears
    // the nearest and not the next.
    for (u32 step = 0; step < kOriginSteps; ++step) {
        const f32 distance = field.distance(origin);
        if (distance >= clearance) {
            break;
        }
        const Vec3 gradient{field.distance(origin + Vec3{voxel, 0.0F, 0.0F}) -
                                field.distance(origin - Vec3{voxel, 0.0F, 0.0F}),
                            field.distance(origin + Vec3{0.0F, voxel, 0.0F}) -
                                field.distance(origin - Vec3{0.0F, voxel, 0.0F}),
                            field.distance(origin + Vec3{0.0F, 0.0F, voxel}) -
                                field.distance(origin - Vec3{0.0F, 0.0F, voxel})};
        origin = origin + (normalized_or(gradient, probe.normal) * (clearance - distance));
    }
    return origin;
}

/// One probe ray from `origin` through the world tier `screen_world_ray` selects: a hit on a
/// surface facing away from the ray answers nothing, as there.
[[nodiscard]] RadianceSample probe_ray(const ScreenResolveSources& sources, Vec3 origin,
                                       Vec3 direction, f32 reach) noexcept {
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 0.0F, -1.0F});
    SceneHit hit;
    if (sources.hardware != nullptr && sources.hardware->available()) {
        (void)sources.hardware->trace(origin, unit, reach, hit);
        if (hit.hit && dot(hit.normal, unit) > 0.0F) {
            return RadianceSample{};
        }
        return sources.hardware->radiance(hit, sources.sky, unit);
    }
    if (sources.software != nullptr) {
        (void)sources.software->trace(origin, unit, reach, hit);
        if (hit.hit && dot(hit.normal, unit) > 0.0F) {
            return RadianceSample{};
        }
        return sources.software->radiance(hit, sources.sky, unit);
    }
    return RadianceSample{};
}

/// The weights `trace_probe` draws its guided rays from, one per cell, from the probe's previous
/// radiance where it has one and the radiance cache's answer in the cell's direction where not.
/// Returns their sum.
[[nodiscard]] f32 guide_weights(const ScreenProbe& probe, bool history,
                                const Vec3 previous[kScreenProbeCoefficients],
                                const ScreenResolveSources& sources,
                                f32 weights[kScreenProbeGuideCells]) noexcept {
    f32 total = 0.0F;
    for (u32 cell = 0; cell < kScreenProbeGuideCells; ++cell) {
        const u32 row = cell / kScreenProbeGuideEdge;
        const u32 column = cell % kScreenProbeGuideEdge;
        const f32 z = (static_cast<f32>(row) + 0.5F) / static_cast<f32>(kScreenProbeGuideEdge);
        const f32 turn =
            (static_cast<f32>(column) + 0.5F) / static_cast<f32>(kScreenProbeGuideEdge);
        const Vec3 direction = hemisphere_direction(probe.normal, z, turn);
        Vec3 radiance{0.0F, 0.0F, 0.0F};
        if (history) {
            radiance = screen_probe_evaluate(previous, direction, kRadianceBands);
        } else if (sources.cache != nullptr) {
            radiance =
                sources.cache->sample(probe.position, direction, sources.cache_frame).radiance;
        }
        weights[cell] = math::max(luminance_of(radiance), 0.0F) * z;
        total += weights[cell];
    }
    return total;
}

/// The defensive mixture a probe draws its rays from: the guide's share of them, and halves of the
/// rest uniform and cosine over the hemisphere.
struct Mixture {
    const f32* weights = nullptr;
    f32 total = 0.0F;
    f32 guided = 0.0F;
    f32 uniform_share = 0.5F;
    f32 cosine_share = 0.5F;
};

[[nodiscard]] Mixture make_mixture(const f32 weights[kScreenProbeGuideCells], f32 total,
                                   f32 importance) noexcept {
    Mixture mixture;
    mixture.weights = weights;
    mixture.total = total;
    mixture.guided = total > 0.0F ? math::clamp(importance, 0.0F, kMaxGuidedShare) : 0.0F;
    mixture.uniform_share = (1.0F - mixture.guided) * 0.5F;
    mixture.cosine_share = (1.0F - mixture.guided) * 0.5F;
    return mixture;
}

/// The guide cell `u` (a unit float past the other shares, rescaled) falls in, by cumulative
/// weight.
[[nodiscard]] u32 pick_cell(const Mixture& mixture, f32 u0) noexcept {
    const f32 target =
        ((u0 - mixture.uniform_share - mixture.cosine_share) / mixture.guided) * mixture.total;
    f32 running = 0.0F;
    for (u32 cell = 0; cell < kScreenProbeGuideCells; ++cell) {
        running += mixture.weights[cell];
        if (mixture.weights[cell] > 0.0F && target < running) {
            return cell;
        }
    }
    return kScreenProbeGuideCells - 1U;
}

/// A direction's (cos theta, phi / 2 pi) from three unit floats: `u0` picks the technique.
void draw(const Mixture& mixture, f32 u0, f32 u1, f32 u2, f32& z, f32& turn) noexcept {
    z = u1;
    turn = u2;
    if (u0 >= mixture.uniform_share && u0 < mixture.uniform_share + mixture.cosine_share) {
        z = std::sqrt(u1);
    } else if (u0 >= mixture.uniform_share + mixture.cosine_share) {
        const u32 chosen = pick_cell(mixture, u0);
        const u32 row = chosen / kScreenProbeGuideEdge;
        const u32 column = chosen % kScreenProbeGuideEdge;
        z = (static_cast<f32>(row) + u1) / static_cast<f32>(kScreenProbeGuideEdge);
        turn = (static_cast<f32>(column) + u2) / static_cast<f32>(kScreenProbeGuideEdge);
    }
}

/// The mixture's density at a direction, per steradian.
[[nodiscard]] f32 mixture_density(const Mixture& mixture, f32 z, f32 turn) noexcept {
    f32 density = (mixture.uniform_share / math::kTwoPi) + (mixture.cosine_share * z / math::kPi);
    if (mixture.guided > 0.0F) {
        density += mixture.guided * (mixture.weights[guide_cell(z, turn)] / mixture.total) *
                   kGuideCellDensity;
    }
    return density;
}

/// The temporal blend: the history's weight falls with its count, never below 1 / `frames`.
void blend(ScreenProbe& probe, const Vec3 estimate[kScreenProbeCoefficients], bool reprojected,
           const Vec3 previous[kScreenProbeCoefficients], f32 history, f32 frames) noexcept {
    if (!reprojected) {
        for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
            probe.coefficients[k] = estimate[k];
        }
        probe.history = 1.0F;
        return;
    }
    const f32 alpha = math::max(1.0F / (history + 1.0F), 1.0F / frames);
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        probe.coefficients[k] = previous[k] + ((estimate[k] - previous[k]) * alpha);
    }
    probe.history = math::min(history + 1.0F, frames);
}

/// What the pixels read before the probe-space filter has run: the coefficients themselves.
void unfiltered(ScreenProbe& probe) noexcept {
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        probe.filtered[k] = probe.coefficients[k];
    }
}

/// The probe-space filter's tent: a neighbour `offset` tiles away along one axis, within `reach`.
[[nodiscard]] f32 filter_tent(i32 offset, u32 reach) noexcept {
    const auto distance = static_cast<f32>(offset < 0 ? -offset : offset);
    return 1.0F - (distance / static_cast<f32>(reach + 1U));
}

/// The uniform tile a probe stands in: its own, or the one holding its half-tile cell's centre.
void probe_tile(const ScreenProbeLayout& layout, const ScreenProbe& probe, u32& x,
                u32& y) noexcept {
    const u32 uniform = layout.uniform_count();
    const u32 pixel = probe.id < uniform ? layout.uniform_pixel(probe.id)
                                         : layout.adaptive_pixel(probe.id - uniform);
    x = std::min((pixel % layout.width) / layout.spacing, layout.tiles_x - 1U);
    y = std::min((pixel / layout.width) / layout.spacing, layout.tiles_y - 1U);
}

/// One probe's `filtered` coefficients from its own and its uniform neighbours' coefficients.
void filter_probe(const ScreenProbeLayout& layout, Span<const ScreenProbe> probes,
                  ScreenProbe& probe, const ScreenProbeSettings& settings) noexcept {
    Vec3 sum[kScreenProbeCoefficients];
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        sum[k] = probe.coefficients[k];
    }
    f32 total = 1.0F;
    u32 tile_x = 0;
    u32 tile_y = 0;
    probe_tile(layout, probe, tile_x, tile_y);
    const auto reach = static_cast<i32>(settings.filter_tiles);
    for (i32 dy = -reach; dy <= reach; ++dy) {
        const i32 y = static_cast<i32>(tile_y) + dy;
        for (i32 dx = -reach; dx <= reach; ++dx) {
            const i32 x = static_cast<i32>(tile_x) + dx;
            if (x < 0 || y < 0 || std::cmp_greater_equal(x, layout.tiles_x) ||
                std::cmp_greater_equal(y, layout.tiles_y)) {
                continue;
            }
            const u32 tile = (static_cast<u32>(y) * layout.tiles_x) + static_cast<u32>(x);
            const ScreenProbe& neighbour = probes[tile];
            if (tile == probe.id || neighbour.depth <= 0.0F) {
                continue;
            }
            const f32 near = 1.0F - (length(neighbour.position - probe.position) /
                                     (settings.filter_reach * probe.depth));
            const f32 weight =
                filter_tent(dx, settings.filter_tiles) * filter_tent(dy, settings.filter_tiles) *
                math::max(near, 0.0F) *
                screen_probe_geometry_weight(neighbour.position, neighbour.normal, probe.position,
                                             probe.normal, probe.depth, settings.plane_tolerance);
            if (weight <= 0.0F) {
                continue;
            }
            for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
                sum[k] = sum[k] + (neighbour.coefficients[k] * weight);
            }
            total += weight;
        }
    }
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        probe.filtered[k] = sum[k] / total;
    }
}

/// One of the field's sky-visibility rays from the probe in the far field; one elsewhere.
[[nodiscard]] f32 probe_openness(const ScreenProbe& probe, Vec3 camera,
                                 const ScreenResolveSettings& settings,
                                 const ScreenResolveSources& sources) noexcept {
    const f32 far = far_field_weight(length(probe.position - camera), settings.error_targets);
    if (far <= 0.0F || sources.field == nullptr || settings.sky_visibility_rays == 0) {
        return 1.0F;
    }
    const u32 count = settings.sky_visibility_rays;
    const auto index = static_cast<u32>((sources.frame + probe.id) % count);
    return sources.field->sky_visibility_ray(probe.position, probe.normal, index, count);
}

/// What the probes about a pixel add up to, by weight; and the one nearest its plane.
struct Gathered {
    Vec3 irradiance{0.0F, 0.0F, 0.0F};
    Vec3 glossy{0.0F, 0.0F, 0.0F};
    Vec3 cache{0.0F, 0.0F, 0.0F};
    f32 cache_confidence = 0.0F;
    f32 openness = 0.0F;
    f32 total = 0.0F;
    const ScreenProbe* nearest = nullptr;
    f32 nearest_plane = 1.0e30F;
};

/// The pixel's diffuse sources combined: the cache (with `cache_blend`), the probes, the far sky.
[[nodiscard]] ResolveResult combine_diffuse(const Gathered& gathered, Vec3 normal,
                                            f32 camera_distance, RadianceSource tier,
                                            const ScreenResolveSettings& settings,
                                            const ScreenResolveSources& sources) noexcept {
    RadianceSample diffuse[3];
    usize count = 0;
    if (settings.probes.cache_blend && sources.cache != nullptr) {
        diffuse[count] = RadianceSample{gathered.cache, gathered.cache_confidence,
                                        RadianceSource::RadianceCache};
        count += 1;
    }
    diffuse[count] = RadianceSample{gathered.irradiance, 1.0F, tier};
    count += 1;
    RadianceSample sky;
    sky.source = RadianceSource::Sky;
    const f32 far = far_field_weight(camera_distance, settings.error_targets);
    if (far > 0.0F) {
        sky.radiance = sources.sky.radiance(normal) * gathered.openness;
        sky.confidence = math::clamp(far * gathered.openness, 0.0F, 1.0F);
    }
    diffuse[count] = sky;
    count += 1;
    ResolveResult result = combine(Span<const RadianceSample>{diffuse, count},
                                   exclusion_for(settings.mode, false, false));
    result.radiance = result.radiance * settings.diffuse_intensity;
    return result;
}

/// How well the uniform probes about a cell stand for its surface: their bilinear and geometric
/// weights summed.
[[nodiscard]] f32 uniform_coverage(const ScreenProbeLayout& layout, Span<const ScreenProbe> probes,
                                   u32 pixel, Vec3 position, Vec3 normal, f32 depth,
                                   const ScreenProbeSettings& settings) noexcept {
    const Taps taps = bilinear_taps(pixel % layout.width, pixel / layout.width, layout.spacing,
                                    layout.tiles_x, layout.tiles_y);
    f32 covered = 0.0F;
    for (u32 tap = 0; tap < taps.count; ++tap) {
        const ScreenProbe& candidate = probes[taps.index[tap]];
        if (candidate.depth <= 0.0F) {
            continue;
        }
        covered += taps.weight[tap] *
                   screen_probe_geometry_weight(candidate.position, candidate.normal, position,
                                                normal, depth, settings.plane_tolerance);
    }
    return covered;
}

/// The probes about a pixel weighed: bilinearly over the uniform and the adaptive grids, by plane
/// distance and by normal; normalised, or the one nearest the pixel's plane where none agrees.
[[nodiscard]] Gathered gather_about(const ScreenProbeLayout& layout, Span<const ScreenProbe> probes,
                                    Span<const u32> slots, u32 pixel, Vec3 position, Vec3 normal,
                                    f32 depth, Vec3 reflection, const f32 specular_bands[3],
                                    f32 tolerance) noexcept {
    const u32 x = pixel % layout.width;
    const u32 y = pixel / layout.width;
    Gathered gathered;
    const auto add = [&](const ScreenProbe& probe, f32 bilinear) noexcept {
        if (probe.depth <= 0.0F) {
            return;
        }
        const f32 plane = std::abs(dot(probe.normal, position - probe.position));
        if (plane < gathered.nearest_plane) {
            gathered.nearest_plane = plane;
            gathered.nearest = &probe;
        }
        const f32 weight =
            bilinear * screen_probe_geometry_weight(probe.position, probe.normal, position, normal,
                                                    depth, tolerance);
        if (weight <= 0.0F) {
            return;
        }
        gathered.irradiance =
            gathered.irradiance +
            (screen_probe_evaluate(probe.filtered, normal, kIrradianceBands) * weight);
        gathered.glossy =
            gathered.glossy +
            (screen_probe_evaluate(probe.filtered, reflection, specular_bands) * weight);
        gathered.cache = gathered.cache + (probe.cache * weight);
        gathered.cache_confidence += probe.cache_confidence * weight;
        gathered.openness += probe.openness * weight;
        gathered.total += weight;
    };
    const Taps uniform = bilinear_taps(x, y, layout.spacing, layout.tiles_x, layout.tiles_y);
    for (u32 tap = 0; tap < uniform.count; ++tap) {
        add(probes[uniform.index[tap]], uniform.weight[tap]);
    }
    const Taps cells = bilinear_taps(x, y, layout.cell, layout.cells_x, layout.cells_y);
    for (u32 tap = 0; tap < cells.count; ++tap) {
        const u32 slot = slots[cells.index[tap]];
        if (slot != kScreenProbeNone) {
            add(probes[layout.uniform_count() + slot], cells.weight[tap]);
        }
    }
    if (gathered.total >= kWeightFloor) {
        gathered.irradiance = gathered.irradiance / gathered.total;
        gathered.glossy = gathered.glossy / gathered.total;
        gathered.cache = gathered.cache / gathered.total;
        gathered.cache_confidence /= gathered.total;
        gathered.openness /= gathered.total;
    } else if (gathered.nearest != nullptr) {
        // No probe agrees with the pixel: the one nearest its plane answers alone.
        const ScreenProbe& nearest = *gathered.nearest;
        gathered.irradiance = screen_probe_evaluate(nearest.filtered, normal, kIrradianceBands);
        gathered.glossy = screen_probe_evaluate(nearest.filtered, reflection, specular_bands);
        gathered.cache = nearest.cache;
        gathered.cache_confidence = nearest.cache_confidence;
        gathered.openness = nearest.openness;
    }
    return gathered;
}

}  // namespace

void screen_probe_basis(Vec3 d, f32 out[kScreenProbeCoefficients]) noexcept {
    out[0] = 0.282095F;
    out[1] = 0.488603F * d.y;
    out[2] = 0.488603F * d.z;
    out[3] = 0.488603F * d.x;
    out[4] = 1.092548F * (d.x * d.y);
    out[5] = 1.092548F * (d.y * d.z);
    out[6] = 0.315392F * ((3.0F * (d.z * d.z)) - 1.0F);
    out[7] = 1.092548F * (d.x * d.z);
    out[8] = 0.546274F * ((d.x * d.x) - (d.y * d.y));
}

Vec3 screen_probe_evaluate(const Vec3 coefficients[kScreenProbeCoefficients], Vec3 direction,
                           const f32 band[3]) noexcept {
    f32 basis[kScreenProbeCoefficients];
    screen_probe_basis(direction, basis);
    Vec3 sum{0.0F, 0.0F, 0.0F};
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        sum = sum + (coefficients[k] * (basis[k] * band[kBandOf[k]]));
    }
    return Vec3{math::max(sum.x, 0.0F), math::max(sum.y, 0.0F), math::max(sum.z, 0.0F)};
}

u32 ScreenProbeLayout::uniform_pixel(u32 tile) const noexcept {
    const u32 x = std::min(((tile % tiles_x) * spacing) + (spacing / 2U), width - 1U);
    const u32 y = std::min(((tile / tiles_x) * spacing) + (spacing / 2U), height - 1U);
    return (y * width) + x;
}

u32 ScreenProbeLayout::adaptive_pixel(u32 cell_index) const noexcept {
    const u32 x = std::min(((cell_index % cells_x) * cell) + (cell / 2U), width - 1U);
    const u32 y = std::min(((cell_index / cells_x) * cell) + (cell / 2U), height - 1U);
    return (y * width) + x;
}

ScreenProbeLayout screen_probe_layout(u32 width, u32 height, u32 spacing) noexcept {
    ScreenProbeLayout layout;
    if (width == 0 || height == 0 || spacing == 0) {
        return layout;
    }
    layout.width = width;
    layout.height = height;
    layout.spacing = std::max(spacing, 2U);
    layout.tiles_x = (width + layout.spacing - 1U) / layout.spacing;
    layout.tiles_y = (height + layout.spacing - 1U) / layout.spacing;
    layout.cell = layout.spacing / 2U;
    layout.cells_x = (width + layout.cell - 1U) / layout.cell;
    layout.cells_y = (height + layout.cell - 1U) / layout.cell;
    layout.capacity = layout.uniform_count();
    return layout;
}

u32 screen_probe_rays(const ScreenResolveSettings& settings) noexcept {
    return std::clamp(settings.probes.rays, 1U, kScreenProbeMaxRays);
}

bool screen_probe_traces(u32 id, u64 frame, f32 density) noexcept {
    if (density >= 1.0F) {
        return true;
    }
    return screen_unit(screen_hash(id, frame, kScreenStreamProbeUpdate)) < density;
}

u32 screen_probe_grid_spacing(u32 spacing_pixels, u32 divisor) noexcept {
    return std::max(spacing_pixels / std::max(divisor, 1U), 2U);
}

f32 screen_probe_geometry_weight(Vec3 probe_position, Vec3 probe_normal, Vec3 position, Vec3 normal,
                                 f32 depth, f32 plane_tolerance) noexcept {
    const f32 plane = std::abs(dot(probe_normal, position - probe_position));
    const f32 reach = plane_tolerance * depth;
    if (reach <= 0.0F) {
        return 0.0F;
    }
    const f32 near = math::max(0.0F, 1.0F - (plane / reach));
    f32 facing = math::max(0.0F, dot(probe_normal, normal));
    facing *= facing;
    facing *= facing;
    facing *= facing;
    return near * facing;
}

Status ScreenProbeGather::resize(u32 width, u32 height, u32 spacing) noexcept {
    const ScreenProbeLayout layout = screen_probe_layout(width, height, spacing);
    if (layout.uniform_count() == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "ScreenProbeGather::resize: a grid with cells and a spacing");
    }
    if (layout.width == layout_.width && layout.height == layout_.height &&
        layout.spacing == layout_.spacing && placed_) {
        return ok();
    }
    for (u32 slot = 0; slot < 2; ++slot) {
        if (Status sized = probes_[slot].resize(layout.records()); !sized) {
            return sized;
        }
        if (Status sized = slots_[slot].resize(layout.cell_count()); !sized) {
            return sized;
        }
        adaptive_count_[slot] = 0;
    }
    layout_ = layout;
    placed_ = false;
    previous_valid_ = false;
    return ok();
}

const ScreenProbe* ScreenProbeGather::adaptive(u32 cell_index) const noexcept {
    if (cell_index >= slots_[slot_].size() || slots_[slot_][cell_index] == kScreenProbeNone) {
        return nullptr;
    }
    return &probes_[slot_][layout_.uniform_count() + slots_[slot_][cell_index]];
}

Status ScreenProbeGather::place(const ScreenSurfaces& surfaces,
                                const ScreenProbeSettings& settings) noexcept {
    const usize cells = static_cast<usize>(layout_.width) * layout_.height;
    if (layout_.uniform_count() == 0 || surfaces.width != layout_.width ||
        surfaces.height != layout_.height || surfaces.depth.size() != cells ||
        surfaces.position.size() != cells || surfaces.normal.size() != cells) {
        return fail(ErrorCode::InvalidArgument,
                    "ScreenProbeGather::place: the surfaces of the grid it was sized for");
    }
    // Last frame's probes become the history; a first frame has none.
    previous_valid_ = placed_ && !reset_;
    reset_ = false;
    if (placed_) {
        slot_ = 1U - slot_;
    }
    placed_ = true;
    stats_ = ScreenProbeStats{};
    Array<ScreenProbe>& probes = probes_[slot_];
    const u32 uniform = layout_.uniform_count();
    for (u32 tile = 0; tile < uniform; ++tile) {
        const u32 pixel = layout_.uniform_pixel(tile);
        ScreenProbe probe;
        probe.id = tile;
        if (surfaces.depth[pixel] > 0.0F) {
            probe.position = surfaces.position[pixel];
            probe.depth = surfaces.depth[pixel];
            probe.normal = normalized_or(surfaces.normal[pixel], Vec3{0.0F, 1.0F, 0.0F});
            stats_.uniform += 1;
        }
        probes[tile] = probe;
    }

    // An adaptive probe where the uniform ones cannot be interpolated, in cell order.
    Array<u32>& slots = slots_[slot_];
    u32 adaptive = 0;
    for (u32 cell = 0; cell < layout_.cell_count(); ++cell) {
        slots[cell] = kScreenProbeNone;
        const u32 pixel = layout_.adaptive_pixel(cell);
        const f32 depth = surfaces.depth[pixel];
        if (depth <= 0.0F) {
            continue;
        }
        const Vec3 position = surfaces.position[pixel];
        const Vec3 normal = normalized_or(surfaces.normal[pixel], Vec3{0.0F, 1.0F, 0.0F});
        const f32 covered =
            uniform_coverage(layout_, probes.span(), pixel, position, normal, depth, settings);
        if (covered >= settings.adaptive_threshold) {
            continue;
        }
        if (adaptive >= layout_.capacity) {
            stats_.refused += 1;
            continue;
        }
        ScreenProbe probe;
        probe.id = uniform + cell;
        probe.position = position;
        probe.depth = depth;
        probe.normal = normal;
        probes[uniform + adaptive] = probe;
        slots[cell] = adaptive;
        adaptive += 1;
    }
    adaptive_count_[slot_] = adaptive;
    stats_.adaptive = adaptive;
    return ok();
}

bool ScreenProbeGather::reproject(const ScreenProbe& probe, const ScreenResolveSources& sources,
                                  const ScreenProbeSettings& settings,
                                  Vec3 coefficients[kScreenProbeCoefficients], f32& history,
                                  const ScreenProbe*& matched) const noexcept {
    const ScreenHistoryInputs& inputs = *sources.history;
    if (screen_invalidated_at(inputs.invalidated, probe.position)) {
        return false;
    }
    u32 x = 0;
    u32 y = 0;
    if (!screen_landing(probe.position, inputs, layout_.width, layout_.height, x, y)) {
        return false;
    }
    // THE ONE PREVIOUS PROBE THAT STANDS FOR THIS ONE BEST, not a blend of them: a blend would
    // mix a probe's neighbours into its history every frame, and over a still view's frames that
    // is a blur no estimate asked for (measured: it took the GI test room from 0.093 to 0.099 of
    // the path tracer at three frames of history).
    const u32 previous = 1U - slot_;
    const Array<ScreenProbe>& probes = probes_[previous];
    const Array<u32>& slots = slots_[previous];
    const ScreenProbe* best = nullptr;
    f32 best_weight = 0.0F;
    const auto consider = [&](const ScreenProbe& candidate, f32 bilinear) noexcept {
        if (candidate.depth <= 0.0F) {
            return;
        }
        const f32 weight = bilinear * screen_probe_geometry_weight(
                                          candidate.position, candidate.normal, probe.position,
                                          probe.normal, probe.depth, settings.plane_tolerance);
        if (weight > best_weight) {
            best_weight = weight;
            best = &candidate;
        }
    };
    const Taps uniform = bilinear_taps(x, y, layout_.spacing, layout_.tiles_x, layout_.tiles_y);
    for (u32 tap = 0; tap < uniform.count; ++tap) {
        consider(probes[uniform.index[tap]], uniform.weight[tap]);
    }
    const Taps cells = bilinear_taps(x, y, layout_.cell, layout_.cells_x, layout_.cells_y);
    for (u32 tap = 0; tap < cells.count; ++tap) {
        const u32 slot = slots[cells.index[tap]];
        if (slot != kScreenProbeNone) {
            consider(probes[layout_.uniform_count() + slot], cells.weight[tap]);
        }
    }
    if (best == nullptr || best_weight < kHistoryMatch) {
        return false;
    }
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        coefficients[k] = best->coefficients[k];
    }
    history = best->history;
    matched = best;
    return true;
}

void ScreenProbeGather::trace_probe(ScreenProbe& probe, const ScreenResolveSettings& settings,
                                    const ScreenResolveSources& sources) noexcept {
    const ScreenProbeSettings& shape = settings.probes;
    Vec3 previous[kScreenProbeCoefficients] = {};
    f32 history = 0.0F;
    const ScreenProbe* matched = nullptr;
    const bool reprojected = previous_valid_ && sources.history != nullptr &&
                             !sources.history->previous_depth.empty() &&
                             reproject(probe, sources, shape, previous, history, matched);
    stats_.reprojected += reprojected ? 1U : 0U;

    // BELOW A RAY DENSITY OF ONE, THAT FRACTION OF THE PROBES TRACE A FRAME, and the rest keep the
    // probe they stand for — its radiance and count, its cache answer and its openness — at none
    // of a traced probe's cost. Fewer rays a probe would not do: a probe's trace is mostly what it
    // does once (its history, its stand-in, its guide), and a probe with half its rays cost 0.92
    // of one with all of them. A probe with no history always traces.
    if (reprojected && !screen_probe_traces(probe.id, sources.frame, settings.ray_density)) {
        for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
            probe.coefficients[k] = previous[k];
        }
        probe.history = history;
        probe.cache = matched->cache;
        probe.cache_confidence = matched->cache_confidence;
        probe.openness = matched->openness;
        unfiltered(probe);
        stats_.carried += 1;
        return;
    }
    const RadianceSample stand_in = cache_at(sources, probe.position, probe.normal);
    probe.cache = stand_in.radiance;
    probe.cache_confidence = stand_in.confidence;

    // The defensive mixture: uniform and cosine halves of what the guide does not take.
    f32 weights[kScreenProbeGuideCells];
    const f32 guide_total = guide_weights(probe, reprojected, previous, sources, weights);
    const Mixture mixture = make_mixture(weights, guide_total, shape.importance_fraction);

    const u32 rays = screen_probe_rays(settings);
    const u32 excluded = exclusion_for(settings.mode, false, false);
    Vec3 sum[kScreenProbeCoefficients] = {};
    const Vec3 origin = probe_origin(sources, probe, settings.origin_lift_metres);
    for (u32 ray = 0; ray < rays; ++ray) {
        const u32 hash =
            screen_hash((probe.id * kScreenProbeMaxRays) + ray, sources.frame, kScreenStreamProbe);
        f32 z = 0.0F;
        f32 turn = 0.0F;
        draw(mixture, screen_unit(hash), screen_unit(hash * 0x27220A95U),
             screen_unit(hash * 0x9E3779B9U), z, turn);
        const f32 density = mixture_density(mixture, z, turn);
        if (density <= 0.0F) {
            continue;
        }
        const Vec3 direction = hemisphere_direction(probe.normal, z, turn);
        const RadianceSample sample = screen_gather_sample(
            probe_ray(sources, origin, direction, settings.max_ray_distance_metres), stand_in,
            excluded);
        if (sample.source == RadianceSource::None) {
            continue;
        }
        f32 basis[kScreenProbeCoefficients];
        screen_probe_basis(direction, basis);
        const f32 weight = 1.0F / (density * static_cast<f32>(rays));
        for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
            sum[k] = sum[k] + (sample.radiance * (basis[k] * weight));
        }
    }
    stats_.rays += rays;
    blend(probe, sum, reprojected, previous, history,
          static_cast<f32>(std::max(shape.history_frames, 1U)));
    unfiltered(probe);
    probe.openness = probe_openness(probe, camera_, settings, sources);
}

void ScreenProbeGather::trace(const ScreenSurfaces& surfaces, const ScreenResolveSettings& settings,
                              const ScreenResolveSources& sources) noexcept {
    camera_ = surfaces.camera;
    Array<ScreenProbe>& probes = probes_[slot_];
    const u32 records = layout_.uniform_count() + adaptive_count_[slot_];
    for (u32 index = 0; index < records; ++index) {
        ScreenProbe& probe = probes[index];
        if (probe.depth <= 0.0F) {
            continue;
        }
        trace_probe(probe, settings, sources);
    }
}

void ScreenProbeGather::filter(const ScreenProbeSettings& settings) noexcept {
    if (settings.denoiser != ScreenProbeDenoiser::ProbeSpace || settings.filter_tiles == 0 ||
        settings.filter_reach <= 0.0F) {
        return;
    }
    // Every probe reads its neighbours' COEFFICIENTS and writes only its own `filtered`, so the
    // order the probes are filtered in changes nothing — the device filters them all at once.
    Array<ScreenProbe>& probes = probes_[slot_];
    const u32 records = layout_.uniform_count() + adaptive_count_[slot_];
    for (u32 index = 0; index < records; ++index) {
        ScreenProbe& probe = probes[index];
        if (probe.depth <= 0.0F) {
            continue;
        }
        filter_probe(layout_, probes.span(), probe, settings);
    }
}

ScreenPixel ScreenProbeGather::integrate(u32 pixel, Vec3 position, Vec3 normal, f32 roughness,
                                         f32 depth, f32 camera_distance, Vec3 view_direction,
                                         const ScreenResolveSettings& settings,
                                         const ScreenResolveSources& sources) const noexcept {
    ScreenPixel answer;
    const Vec3 reflection = screen_reflect_view(view_direction, normal);
    const f32 rough = math::clamp(roughness, 0.0F, 1.0F);
    f32 specular_bands[3];
    for (u32 band = 0; band < 3; ++band) {
        specular_bands[band] = 1.0F + ((kIrradianceBands[band] - 1.0F) * rough);
    }
    const Gathered gathered =
        gather_about(layout_, probes_[slot_].span(), slots_[slot_].span(), pixel, position, normal,
                     depth, reflection, specular_bands, settings.probes.plane_tolerance);
    if (gathered.total < kWeightFloor && gathered.nearest == nullptr) {
        return answer;
    }
    const Vec3 glossy = gathered.glossy;
    const RadianceSource tier = tier_source(sources);
    answer.diffuse = combine_diffuse(gathered, normal, camera_distance, tier, settings, sources);

    // The rough specular from the same coefficients; a smooth surface also spends its own ray.
    const RadianceSample probe_specular{glossy, math::clamp(roughness, 0.15F, 1.0F), tier};
    RadianceSample specular[2];
    usize specular_count = 0;
    const u32 rays = reflection_ray_count(roughness, settings.reflection_thresholds);
    answer.traced_specular = rays > 0 && screen_traces(pixel, sources.frame, settings.ray_density);
    if (answer.traced_specular) {
        const u32 hash = screen_hash(pixel, sources.frame, kScreenStreamSpecular);
        const Vec3 direction = reflection_lobe_direction(reflection, normal, roughness, hash % rays,
                                                         rays, static_cast<u32>(sources.frame));
        specular[specular_count] = screen_gather_sample(
            screen_world_ray(sources, position, normal, direction, settings.origin_lift_metres,
                             settings.max_ray_distance_metres),
            probe_specular, 0U);
        specular_count += 1;
    }
    specular[specular_count] = probe_specular;
    specular_count += 1;
    answer.specular = combine(Span<const RadianceSample>{specular, specular_count}, 0);
    answer.specular.radiance = answer.specular.radiance * settings.specular_intensity;
    return answer;
}

}  // namespace cy::rendering::gi
