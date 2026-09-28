// SPDX-License-Identifier: MIT
// Volumetric fog on the host, whole columns at a time: the march against the single-scattering
// integral and a closed form, the empty medium's exact identity, and the lookup against the march.
// `integration.rendering_fog_march`.
//
// These cases march every column of a volume, or quadrature a ray at twenty thousand points, which
// is a few milliseconds of CPU and over the unit suite's budget; the medium and the constants they
// build on stay in `unit.rendering_fog`. `single_scattering_reference` is the single-scattering
// equation by brute-force quadrature with nothing of the march in it; the case that holds
// `integrate_fog_column` to it is what says the froxel integration is right rather than merely
// consistent with itself. The device suite holds the device's volume to both.

#include <cy/rendering/fog/medium.h>
#include <cy/rendering/fog/volume.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec3;
using cy::Vec4;
using namespace cy::rendering::fog;

FogSettings small_settings() noexcept {
    FogSettings settings;
    settings.volume.width = 16;
    settings.volume.height = 9;
    settings.volume.depth = 32;
    settings.volume.near_plane = 0.1F;
    settings.volume.far_plane = 40.0F;
    settings.volume.depth_exponent = 2.0F;
    settings.steps_per_slice = 4;
    return settings;
}

FogLight daylight() noexcept {
    FogLight light;
    light.to_sun = normalize(Vec3{0.3F, 0.6F, -0.74F});
    light.sun_illuminance = Vec3{1.0F, 0.95F, 0.85F};
    light.ambient_radiance = Vec3{0.05F, 0.07F, 0.1F};
    return light;
}

f32 relative_error(Vec3 got, Vec3 want) noexcept {
    const f32 scale = cy::max_component(want) > 1e-9F ? cy::max_component(want) : 1.0F;
    const Vec3 delta = cy::cwise_abs(got - want);
    return cy::max_component(delta) / scale;
}

/// A march of every column into a texture laid out as the device lays it out.
std::vector<Vec4> march_volume(const FogSettings& settings, const FogView& view,
                               const FogLight& light, const FogMedium& medium) {
    const FogTextureExtent extent = fog_texture_extent(settings.volume);
    std::vector<Vec4> texels(static_cast<usize>(extent.width) * extent.height, Vec4{});
    const cy::Expected<FogConstants, cy::Error> constants =
        pack_fog_constants(settings, view, light, FogShadow{}, medium);
    if (!constants.has_value()) {
        return {};
    }
    for (u32 texel = 0; texel < kFogHeaderTexels; ++texel) {
        texels[texel] = constants->words[texel];
    }
    const cy::rendering::FroxelVolume& volume = settings.volume;
    std::vector<Vec3> t(volume.depth);
    std::vector<Vec3> s(volume.depth);
    for (u32 y = 0; y < volume.height; ++y) {
        for (u32 x = 0; x < volume.width; ++x) {
            if (!integrate_fog_column(settings, view, light, FogShadow{}, HostShadowMap{}, medium,
                                      x, y, cy::Span<Vec3>(t), cy::Span<Vec3>(s))
                     .has_value()) {
                return {};
            }
            for (u32 slice = 0; slice < volume.depth; ++slice) {
                const usize row = 1U + (static_cast<usize>(slice) * volume.height) + y;
                texels[(row * extent.width) + x] = Vec4{t[slice].x, t[slice].y, t[slice].z, 1.0F};
                texels[(row * extent.width) + volume.width + x] =
                    Vec4{s[slice].x, s[slice].y, s[slice].z, 1.0F};
            }
        }
    }
    return texels;
}

}  // namespace

CY_TEST_CASE("the march converges to the single-scattering integral") {
    // A height fog that thins upward, and a glowing, forward-scattering sphere in the view: the
    // medium varies along every ray, so a march that sampled one point a slice, or dropped the
    // transmittance in front of a sub-step, or integrated a sub-step as a point sample, is off by
    // far more than the tolerance.
    const FogSettings settings = small_settings();
    FogView view;
    view.world_offset = Vec3{0.0F, 3.0F, 0.0F};
    const FogLight light = daylight();
    FogMedium medium;
    medium.height.extinction = 0.06F;
    medium.height.base_height = 0.0F;
    medium.height.scale_height = 4.0F;
    medium.volume_count = 1;
    medium.volumes[0].shape = FogVolumeShape::Sphere;
    medium.volumes[0].centre = Vec3{2.0F, 3.0F, -14.0F};
    medium.volumes[0].size = Vec3{4.0F, 0.0F, 0.0F};
    medium.volumes[0].edge = 1.0F;
    medium.volumes[0].extinction = 0.15F;
    medium.volumes[0].albedo = Vec3{0.8F, 0.7F, 0.6F};
    medium.volumes[0].anisotropy = 0.5F;
    medium.volumes[0].emission = Vec3{0.004F, 0.002F, 0.001F};

    const cy::rendering::FroxelVolume& volume = settings.volume;
    std::vector<Vec3> t(volume.depth);
    std::vector<Vec3> s(volume.depth);
    f32 worst_s = 0.0F;
    f32 worst_t = 0.0F;
    const u32 columns[][2] = {{0, 0}, {5, 3}, {11, 4}, {8, 6}, {15, 8}};
    for (const auto& column : columns) {
        CY_REQUIRE(integrate_fog_column(settings, view, light, FogShadow{}, HostShadowMap{}, medium,
                                        column[0], column[1], cy::Span<Vec3>(t), cy::Span<Vec3>(s))
                       .has_value());
        const Vec3 direction = fog_column_direction(settings, view, column[0], column[1]);
        const f32 cosine = dot(direction, view.forward);
        for (const u32 slice : {0U, 3U, 11U, 20U, 31U}) {
            const f32 distance = cy::rendering::froxel_slice_depth(volume, slice) / cosine;
            const FogAtPoint reference = single_scattering_reference(
                view, light, FogShadow{}, HostShadowMap{}, medium, direction, distance, 20'000);
            worst_s = cy::math::max(worst_s, relative_error(s[slice], reference.in_scattering));
            worst_t = cy::math::max(worst_t, std::fabs(t[slice].x - reference.transmittance.x));
        }
    }
    std::printf(
        "march against the single-scattering integral: worst in-scattering %.3g, worst "
        "transmittance %.3g\n",
        static_cast<double>(worst_s), static_cast<double>(worst_t));
    CY_CHECK_LT(worst_s, 5e-3F);
    CY_CHECK_LT(worst_t, 1e-3F);

    // And a homogeneous medium against the closed form, S = J (1 - exp(-sigma_t d)) / sigma_t.
    FogMedium uniform;
    uniform.height.extinction = 0.08F;
    uniform.height.base_height = 1000.0F;
    uniform.height.albedo = Vec3{0.9F, 0.9F, 0.9F};
    uniform.height.anisotropy = 0.3F;
    CY_REQUIRE(integrate_fog_column(settings, view, light, FogShadow{}, HostShadowMap{}, uniform, 7,
                                    4, cy::Span<Vec3>(t), cy::Span<Vec3>(s))
                   .has_value());
    const Vec3 direction = fog_column_direction(settings, view, 7, 4);
    const MediumSample point = sample_medium(uniform, Vec3{}, dot(direction, light.to_sun));
    const Vec3 source = (cy::cwise_mul(point.sun_scattering, light.sun_illuminance)) +
                        cy::cwise_mul(point.scattering, light.ambient_radiance);
    const f32 distance =
        cy::rendering::froxel_slice_depth(volume, volume.depth - 1U) / dot(direction, view.forward);
    const Vec3 closed = source * ((1.0F - std::exp(-0.08F * distance)) / 0.08F);
    CY_CHECK_LT(relative_error(s[volume.depth - 1U], closed), 1e-4F);
    CY_CHECK_NEAR(t[volume.depth - 1U].x, std::exp(-0.08F * distance), 1e-5F);
}

CY_TEST_CASE("an empty medium leaves transmittance exactly one and in-scattering exactly zero") {
    const FogSettings settings = small_settings();
    const std::vector<Vec4> texels = march_volume(settings, FogView{}, daylight(), FogMedium{});
    CY_REQUIRE_FALSE(texels.empty());
    const FogTextureExtent extent = fog_texture_extent(settings.volume);
    for (usize index = extent.width; index < texels.size(); ++index) {
        const bool transmittance = (index % extent.width) < settings.volume.width;
        CY_CHECK_EQ(texels[index].x, transmittance ? 1.0F : 0.0F);
        CY_CHECK_EQ(texels[index].z, transmittance ? 1.0F : 0.0F);
    }
    // So the lookup returns the identity bit for bit wherever a surface is.
    for (const Vec3 point : {Vec3{0.3F, -0.2F, -0.05F}, Vec3{-4.0F, 1.0F, -12.5F},
                             Vec3{9.0F, -3.0F, -39.0F}, Vec3{0.0F, 0.0F, -300.0F}}) {
        const FogAtPoint fog = fog_at(cy::Span<const Vec4>(texels), extent, point);
        CY_CHECK(fog.transmittance == (Vec3{1.0F, 1.0F, 1.0F}));
        CY_CHECK(fog.in_scattering == (Vec3{0.0F, 0.0F, 0.0F}));
    }
}

CY_TEST_CASE("the lookup starts at the eye and meets each column at its slices' far edges") {
    const FogSettings settings = small_settings();
    const FogView view;
    const FogLight light = daylight();
    FogMedium medium;
    medium.height.extinction = 0.05F;
    medium.height.base_height = 100.0F;
    const std::vector<Vec4> texels = march_volume(settings, view, light, medium);
    CY_REQUIRE_FALSE(texels.empty());
    const FogTextureExtent extent = fog_texture_extent(settings.volume);
    const cy::Span<const Vec4> span(texels);

    // Behind the eye, and at the eye, nothing.
    const FogAtPoint behind = fog_at(span, extent, Vec3{0.0F, 0.0F, 2.0F});
    CY_CHECK(behind.transmittance == (Vec3{1.0F, 1.0F, 1.0F}));
    const FogAtPoint near = fog_at(span, extent, Vec3{0.0F, 0.0F, -1e-4F});
    CY_CHECK_NEAR(near.transmittance.x, 1.0F, 1e-5F);
    CY_CHECK_NEAR(near.in_scattering.x, 0.0F, 1e-5F);

    // At a column's centre and a slice's far edge, the lookup IS the stored value.
    std::vector<Vec3> t(settings.volume.depth);
    std::vector<Vec3> s(settings.volume.depth);
    CY_REQUIRE(integrate_fog_column(settings, view, light, FogShadow{}, HostShadowMap{}, medium, 6,
                                    5, cy::Span<Vec3>(t), cy::Span<Vec3>(s))
                   .has_value());
    const Vec3 direction = fog_column_direction(settings, view, 6, 5);
    for (const u32 slice : {2U, 9U, 17U}) {
        const f32 depth = cy::rendering::froxel_slice_depth(settings.volume, slice);
        const Vec3 point = direction * (depth / dot(direction, view.forward));
        const FogAtPoint fog = fog_at(span, extent, point);
        CY_CHECK_LT(relative_error(fog.in_scattering, s[slice]), 1e-4F);
        CY_CHECK_NEAR(fog.transmittance.x, t[slice].x, 1e-5F);
    }
    // And the transmittance falls, and the in-scattering grows, with distance along a ray.
    f32 last_t = 1.0F;
    f32 last_s = 0.0F;
    for (u32 step = 0; step < 19U; ++step) {
        const f32 distance = 0.5F * std::pow(1.3F, static_cast<f32>(step));  // 0.5 m to 56 m
        const FogAtPoint fog = fog_at(span, extent, direction * distance);
        CY_CHECK_LE(fog.transmittance.x, last_t);
        CY_CHECK_GE(fog.in_scattering.x, last_s);
        last_t = fog.transmittance.x;
        last_s = fog.in_scattering.x;
    }
}
