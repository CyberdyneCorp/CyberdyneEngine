// SPDX-License-Identifier: MIT
#include <cy/rendering/fog/volume.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering::fog {
namespace {

[[nodiscard]] Vec4 word(Vec3 xyz, f32 w) noexcept {
    return Vec4{xyz.x, xyz.y, xyz.z, w};
}

[[nodiscard]] Vec4 transform(const Mat4& matrix, Vec3 point) noexcept {
    const Vec4 p{point.x, point.y, point.z, 1.0F};
    return Vec4{dot(matrix.row(0), p), dot(matrix.row(1), p), dot(matrix.row(2), p),
                dot(matrix.row(3), p)};
}

[[nodiscard]] f32 lit(f32 reference, f32 stored) noexcept {
    // Reversed Z: the occluder nearest the light holds the LARGEST depth, so a point is lit when
    // its reference is at least what the map stored — the comparison both frames make.
    return reference >= stored ? 1.0F : 0.0F;
}

/// The depth tap `fog_at` and the shader share: which slice's far edge is nearer the point, which
/// is farther, and how far between them the point lies. Slice -1 is the eye.
struct DepthTap {
    i32 near_slice = -1;
    u32 far_slice = 0;
    f32 fraction = 0.0F;
};

[[nodiscard]] DepthTap depth_tap(const FroxelVolume& volume, f32 depth) noexcept {
    DepthTap tap;
    const f32 first = froxel_slice_depth(volume, 0);
    if (depth <= first) {
        tap.fraction = math::saturate(depth / math::max(first, 1e-6F));
        return tap;
    }
    const u32 last = volume.depth - 1U;
    const f32 span = math::max(volume.far_plane - volume.near_plane, 1e-6F);
    const f32 normalised = math::saturate((depth - volume.near_plane) / span);
    const f32 continuous =
        (std::pow(normalised, 1.0F / math::max(volume.depth_exponent, 1.0F)) *
         static_cast<f32>(volume.depth)) -
        1.0F;
    const u32 slice = math::min(static_cast<u32>(math::max(continuous, 0.0F)), last);
    tap.near_slice = static_cast<i32>(slice);
    tap.far_slice = math::min(slice + 1U, last);
    const f32 near_edge = froxel_slice_depth(volume, slice);
    const f32 far_edge = froxel_slice_depth(volume, slice + 1U);
    tap.fraction = slice == last
                       ? 1.0F
                       : math::saturate((depth - near_edge) / math::max(far_edge - near_edge, 1e-6F));
    return tap;
}

struct PlaneTaps {
    u32 x0 = 0;
    u32 x1 = 0;
    u32 y0 = 0;
    u32 y1 = 0;
    f32 tx = 0.0F;
    f32 ty = 0.0F;
};

[[nodiscard]] Vec3 fetch(Span<const Vec4> texels, FogTextureExtent extent, u32 column, u32 row) noexcept {
    return texels[(static_cast<usize>(row) * extent.width) + column].xyz();
}

[[nodiscard]] Vec3 plane(Span<const Vec4> texels, FogTextureExtent extent, const FroxelVolume& volume,
                         const PlaneTaps& taps, u32 slice, u32 which) noexcept {
    const u32 base = which * volume.width;
    const u32 row0 = 1U + (slice * volume.height) + taps.y0;
    const u32 row1 = 1U + (slice * volume.height) + taps.y1;
    const Vec3 bottom = lerp(fetch(texels, extent, base + taps.x0, row0),
                             fetch(texels, extent, base + taps.x1, row0), taps.tx);
    const Vec3 top = lerp(fetch(texels, extent, base + taps.x0, row1),
                          fetch(texels, extent, base + taps.x1, row1), taps.tx);
    return lerp(bottom, top, taps.ty);
}

[[nodiscard]] PlaneTaps plane_taps(const FroxelVolume& volume, f32 ndc_x, f32 ndc_y) noexcept {
    const f32 x = math::clamp((((ndc_x * 0.5F) + 0.5F) * static_cast<f32>(volume.width)) - 0.5F,
                              0.0F, static_cast<f32>(volume.width - 1U));
    const f32 y = math::clamp((((ndc_y * 0.5F) + 0.5F) * static_cast<f32>(volume.height)) - 0.5F,
                              0.0F, static_cast<f32>(volume.height - 1U));
    PlaneTaps taps;
    taps.x0 = static_cast<u32>(x);
    taps.y0 = static_cast<u32>(y);
    taps.x1 = math::min(taps.x0 + 1U, volume.width - 1U);
    taps.y1 = math::min(taps.y0 + 1U, volume.height - 1U);
    taps.tx = x - static_cast<f32>(taps.x0);
    taps.ty = y - static_cast<f32>(taps.y0);
    return taps;
}

/// The air between two distances along a ray, as a homogeneous medium: `fogAirStretch` in
/// volumetric_fog.slang.
struct AirStretch {
    Vec3 extinction{0.0F, 0.0F, 0.0F};
    Vec3 source{0.0F, 0.0F, 0.0F};
};

[[nodiscard]] f32 escape_fraction(f32 x) noexcept {
    return x < 1e-3F ? 1.0F - (0.5F * x) : (1.0F - std::exp(-x)) / x;
}

[[nodiscard]] AirStretch air_stretch(const sky::AerialPerspectiveTable& air, Vec3 direction,
                                     f32 near_distance, f32 far_distance) noexcept {
    const sky::AerialPerspective near_air = air.sample_at(direction * near_distance);
    const sky::AerialPerspective far_air = air.sample_at(direction * far_distance);
    const f32 span = math::max(far_distance - near_distance, 1e-6F);
    AirStretch stretch;
    for (u32 channel = 0; channel < 3; ++channel) {
        const f32 near_t = math::max(near_air.transmittance[channel], 1e-6F);
        const f32 transmitted = math::saturate(far_air.transmittance[channel] / near_t);
        const f32 added =
            math::max(far_air.in_scattering[channel] - near_air.in_scattering[channel], 0.0F) /
            near_t;
        const f32 extinction = -std::log(math::max(transmitted, 1e-6F)) / span;
        stretch.extinction[channel] = extinction;
        stretch.source[channel] = added / (span * escape_fraction(extinction * span));
    }
    return stretch;
}

[[nodiscard]] Vec3 source_term(const MediumSample& medium, const FogLight& light,
                               f32 visibility) noexcept {
    return (cwise_mul(medium.sun_scattering, light.sun_illuminance) * visibility) +
           cwise_mul(medium.scattering, light.ambient_radiance) + medium.emission;
}

}  // namespace

FogView fog_view_from(const Mat4& relative_to_view, f32 fov_y_radians, f32 aspect, Vec3 eye,
                      Vec3 world_offset) noexcept {
    FogView view;
    view.eye = eye;
    view.right = normalize(relative_to_view.row(0).xyz());
    view.up = normalize(relative_to_view.row(1).xyz());
    view.forward = normalize(relative_to_view.row(2).xyz() * -1.0F);
    view.tan_half_fov_y = std::tan(fov_y_radians * 0.5F);
    view.tan_half_fov_x = view.tan_half_fov_y * aspect;
    view.world_offset = world_offset;
    return view;
}

Mat4 shadow_uv_rows(const Mat4& relative_to_shadow_clip, bool flip_v) noexcept {
    const Vec4 r0 = relative_to_shadow_clip.row(0);
    const Vec4 r1 = relative_to_shadow_clip.row(1);
    const Vec4 r2 = relative_to_shadow_clip.row(2);
    const Vec4 r3 = relative_to_shadow_clip.row(3);
    const f32 v_sign = flip_v ? -0.5F : 0.5F;
    const Vec4 u_row{(r0.x * 0.5F) + (r3.x * 0.5F), (r0.y * 0.5F) + (r3.y * 0.5F),
                     (r0.z * 0.5F) + (r3.z * 0.5F), (r0.w * 0.5F) + (r3.w * 0.5F)};
    const Vec4 v_row{(r1.x * v_sign) + (r3.x * 0.5F), (r1.y * v_sign) + (r3.y * 0.5F),
                     (r1.z * v_sign) + (r3.z * 0.5F), (r1.w * v_sign) + (r3.w * 0.5F)};
    Mat4 out = Mat4::zero();
    const Vec4 rows[4] = {u_row, v_row, r2, r3};
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            out.at(row, column) = rows[row][column];
        }
    }
    return out;
}

Expected<FogConstants, Error> pack_fog_constants(const FogSettings& settings, const FogView& view,
                                                 const FogLight& light, const FogShadow& shadow,
                                                 const FogMedium& medium) noexcept {
    const FroxelVolume& volume = settings.volume;
    if (volume.width < 3U || volume.height == 0U || volume.depth == 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "volumetric fog: the volume needs at least three columns (its first row holds "
                    "a six-texel header across two texels a column) and one row and slice");
    }
    if (settings.steps_per_slice == 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "volumetric fog: a slice needs at least one sub-step");
    }
    if (!(volume.far_plane > volume.near_plane) || volume.near_plane < 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "volumetric fog: the volume's far plane must lie beyond its near plane");
    }
    FogConstants constants;
    Vec4* w = constants.words;
    const u32 count = math::min(medium.volume_count, kMaxFogVolumes);
    w[0] = word(view.forward, 1.0F);
    w[1] = word(view.right, view.tan_half_fov_x);
    w[2] = word(view.up, view.tan_half_fov_y);
    w[3] = Vec4{static_cast<f32>(volume.width), static_cast<f32>(volume.height),
                static_cast<f32>(volume.depth), math::max(volume.depth_exponent, 1.0F)};
    w[4] = Vec4{volume.near_plane, volume.far_plane, static_cast<f32>(settings.steps_per_slice),
                0.0F};
    w[5] = word(view.eye, 0.0F);
    w[6] = word(view.world_offset, static_cast<f32>(count));
    w[7] = word(normalize(light.to_sun), shadow.enabled ? 1.0F : 0.0F);
    w[8] = word(light.sun_illuminance, 0.0F);
    w[9] = word(light.ambient_radiance, 0.0F);
    for (u32 row = 0; row < 4; ++row) {
        w[10 + row] = shadow.relative_to_uv.row(row);
    }
    const f32 extent = static_cast<f32>(math::max(shadow.extent, 1U));
    w[14] = Vec4{shadow.bias, extent, 1.0F / extent, 0.0F};
    w[15] = Vec4{math::max(medium.height.extinction, 0.0F), medium.height.base_height,
                 math::max(medium.height.scale_height, 1e-3F), medium.height.anisotropy};
    w[16] = word(medium.height.albedo, 0.0F);
    for (u32 index = 0; index < count; ++index) {
        const FogVolume& fog = medium.volumes[index];
        Vec4* v = w + kFogConstantHeaderWords + (index * kFogVolumeWords);
        v[0] = word(fog.centre, static_cast<f32>(fog.shape));
        v[1] = word(fog.size, math::max(fog.edge, 0.0F));
        v[2] = word(fog.albedo, math::max(fog.extinction, 0.0F));
        v[3] = word(fog.emission, fog.anisotropy);
    }
    return constants;
}

FogTextureExtent fog_texture_extent(const FroxelVolume& volume) noexcept {
    return FogTextureExtent{volume.width * 2U, 1U + (volume.height * volume.depth)};
}

u64 fog_table_words(const FroxelVolume& volume) noexcept {
    return sky::kAerialPerspectiveHeaderWords +
           (static_cast<u64>(volume.width) * volume.height * volume.depth * 2U);
}

FogAtPoint fog_at(Span<const Vec4> texels, FogTextureExtent extent,
                  Vec3 relative_position) noexcept {
    FogAtPoint result;
    if (extent.width < 2U * 3U || texels.size() < static_cast<usize>(extent.width) * extent.height ||
        texels[0].w < 0.5F) {
        return result;
    }
    const Vec3 forward = texels[0].xyz();
    const Vec3 right = texels[1].xyz();
    const Vec3 up = texels[2].xyz();
    FroxelVolume volume;
    volume.width = static_cast<u32>(texels[3].x);
    volume.height = static_cast<u32>(texels[3].y);
    volume.depth = static_cast<u32>(texels[3].z);
    volume.depth_exponent = texels[3].w;
    volume.near_plane = texels[4].x;
    volume.far_plane = texels[4].y;
    const Vec3 offset = relative_position - texels[5].xyz();
    const f32 depth = dot(offset, forward);
    if (depth <= 0.0F || volume.width == 0U || volume.height == 0U || volume.depth == 0U) {
        return result;
    }
    const f32 ndc_x = dot(offset, right) / (depth * texels[1].w);
    const f32 ndc_y = dot(offset, up) / (depth * texels[2].w);
    const PlaneTaps taps = plane_taps(volume, ndc_x, ndc_y);
    const DepthTap tap = depth_tap(volume, depth);
    Vec3 near_t{1.0F, 1.0F, 1.0F};
    Vec3 near_s{0.0F, 0.0F, 0.0F};
    if (tap.near_slice >= 0) {
        near_t = plane(texels, extent, volume, taps, static_cast<u32>(tap.near_slice), 0U);
        near_s = plane(texels, extent, volume, taps, static_cast<u32>(tap.near_slice), 1U);
    }
    const Vec3 far_t = plane(texels, extent, volume, taps, tap.far_slice, 0U);
    const Vec3 far_s = plane(texels, extent, volume, taps, tap.far_slice, 1U);
    result.transmittance = lerp(near_t, far_t, tap.fraction);
    result.in_scattering = lerp(near_s, far_s, tap.fraction);
    return result;
}

f32 fog_shadow_visibility(const FogShadow& shadow, const HostShadowMap& map,
                          Vec3 relative_position) noexcept {
    if (!shadow.enabled || map.extent == 0U ||
        map.depth.size() < static_cast<usize>(map.extent) * map.extent) {
        return 1.0F;
    }
    const Vec4 q = transform(shadow.relative_to_uv, relative_position);
    if (q.w <= 0.0F) {
        return 1.0F;
    }
    const f32 u = q.x / q.w;
    const f32 v = q.y / q.w;
    if (u < 0.0F || u > 1.0F || v < 0.0F || v > 1.0F) {
        return 1.0F;
    }
    const f32 reference = (q.z / q.w) + shadow.bias;
    const f32 size = static_cast<f32>(map.extent);
    const f32 tx = (u * size) - 0.5F;
    const f32 ty = (v * size) - 0.5F;
    const f32 fx = std::floor(tx);
    const f32 fy = std::floor(ty);
    const auto clamp_texel = [&](f32 value) {
        return static_cast<u32>(math::clamp(value, 0.0F, size - 1.0F));
    };
    const u32 x0 = clamp_texel(fx);
    const u32 x1 = clamp_texel(fx + 1.0F);
    const u32 y0 = clamp_texel(fy);
    const u32 y1 = clamp_texel(fy + 1.0F);
    const auto at = [&](u32 x, u32 y) {
        return lit(reference, map.depth[(static_cast<usize>(y) * map.extent) + x]);
    };
    const f32 ax = tx - fx;
    const f32 ay = ty - fy;
    const f32 top = math::lerp(at(x0, y0), at(x1, y0), ax);
    const f32 bottom = math::lerp(at(x0, y1), at(x1, y1), ax);
    return math::lerp(top, bottom, ay);
}

Vec3 fog_column_direction(const FogSettings& settings, const FogView& view, u32 x,
                          u32 y) noexcept {
    const FroxelVolume& volume = settings.volume;
    const f32 ndc_x =
        (((static_cast<f32>(x) + 0.5F) / static_cast<f32>(math::max(volume.width, 1U))) * 2.0F) -
        1.0F;
    const f32 ndc_y =
        (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(math::max(volume.height, 1U))) * 2.0F) -
        1.0F;
    return normalize(view.forward + (view.right * (ndc_x * view.tan_half_fov_x)) +
                     (view.up * (ndc_y * view.tan_half_fov_y)));
}

Status integrate_fog_column(const FogSettings& settings, const FogView& view,
                            const FogLight& light, const FogShadow& shadow,
                            const HostShadowMap& map, const FogMedium& medium, u32 x, u32 y,
                            Span<Vec3> transmittance, Span<Vec3> in_scattering,
                            const sky::AerialPerspectiveTable* air) noexcept {
    const FroxelVolume& volume = settings.volume;
    if (x >= volume.width || y >= volume.height || settings.steps_per_slice == 0U ||
        transmittance.size() != volume.depth || in_scattering.size() != volume.depth) {
        return fail(ErrorCode::InvalidArgument,
                    "volumetric fog: the column or its output spans are not the volume's");
    }
    const bool with_air = air != nullptr && air->built();
    const Vec3 direction = fog_column_direction(settings, view, x, y);
    const f32 forward_cosine = math::max(dot(direction, view.forward), 1.0e-3F);
    const f32 cos_to_sun = dot(direction, normalize(light.to_sun));
    const f32 steps = static_cast<f32>(settings.steps_per_slice);
    f32 previous = 0.0F;
    Vec3 carried{1.0F, 1.0F, 1.0F};
    Vec3 scattered{0.0F, 0.0F, 0.0F};
    for (u32 slice = 0; slice < volume.depth; ++slice) {
        const f32 distance = froxel_slice_depth(volume, slice) / forward_cosine;
        const f32 step = math::max(distance - previous, 0.0F) / steps;
        const AirStretch stretch =
            with_air ? air_stretch(*air, direction, previous, distance) : AirStretch{};
        for (u32 sub = 0; sub < settings.steps_per_slice; ++sub) {
            const f32 along = previous + ((static_cast<f32>(sub) + 0.5F) * step);
            const Vec3 point = view.eye + (direction * along);
            const MediumSample sample =
                sample_medium(medium, point + view.world_offset, cos_to_sun);
            const f32 visibility = fog_shadow_visibility(shadow, map, point);
            const Vec3 source = source_term(sample, light, visibility) + stretch.source;
            // `integrate_froxel` per channel: the fog's grey extinction plus the air's.
            for (u32 channel = 0; channel < 3; ++channel) {
                const ScatteringStep next = integrate_froxel(
                    Vec3{source[channel], 0.0F, 0.0F},
                    math::max(sample.extinction, 0.0F) + stretch.extinction[channel], step,
                    Vec3{scattered[channel], 0.0F, 0.0F}, carried[channel]);
                scattered[channel] = next.scattering.x;
                carried[channel] = next.transmittance;
            }
        }
        previous = distance;
        transmittance[slice] = carried;
        in_scattering[slice] = scattered;
    }
    return ok();
}

FogAtPoint single_scattering_reference(const FogView& view, const FogLight& light,
                                       const FogShadow& shadow, const HostShadowMap& map,
                                       const FogMedium& medium, Vec3 direction, f32 distance,
                                       u32 steps) noexcept {
    FogAtPoint result;
    const u32 count = math::max(steps, 1U);
    const Vec3 unit = normalize(direction);
    const f32 cos_to_sun = dot(unit, normalize(light.to_sun));
    const f64 ds = static_cast<f64>(distance) / static_cast<f64>(count);
    // Optical depth and radiance accumulated in double: the reference's own rounding must be well
    // below the tolerance the march is held to, over tens of thousands of steps.
    f64 optical_depth = 0.0;
    f64 radiance[3] = {0.0, 0.0, 0.0};
    for (u32 index = 0; index < count; ++index) {
        const f64 along = (static_cast<f64>(index) + 0.5) * ds;
        const Vec3 point = view.eye + (unit * static_cast<f32>(along));
        const MediumSample sample = sample_medium(medium, point + view.world_offset, cos_to_sun);
        const Vec3 source = source_term(sample, light, fog_shadow_visibility(shadow, map, point));
        const f64 half = static_cast<f64>(sample.extinction) * ds * 0.5;
        const f64 carried = std::exp(-(optical_depth + half));
        radiance[0] += carried * static_cast<f64>(source.x) * ds;
        radiance[1] += carried * static_cast<f64>(source.y) * ds;
        radiance[2] += carried * static_cast<f64>(source.z) * ds;
        optical_depth += half * 2.0;
    }
    const auto remaining = static_cast<f32>(std::exp(-optical_depth));
    result.transmittance = Vec3{remaining, remaining, remaining};
    result.in_scattering = Vec3{static_cast<f32>(radiance[0]), static_cast<f32>(radiance[1]),
                                static_cast<f32>(radiance[2])};
    return result;
}

}  // namespace cy::rendering::fog
