// SPDX-License-Identifier: MIT
// Volumetric fog composited with the atmosphere's froxel table, on the host.
// `integration.rendering_fog_air`.
//
// "Aerial perspective from the physical atmosphere SHALL be composited consistently with fog rather
// than applied separately." The table variant of the march reads each slice's stretch of air out of
// `sky::AerialPerspectiveTable` and marches it WITH the fog as one more homogeneous medium. Three
// things follow, and each is a case: an empty fog reproduces the air table; a disabled air table
// leaves the fog exactly as it was; and together the two transmit the product of what each
// transmits and add less than the sum of what each adds, because each medium dims the other's light.
//
// Integration rather than unit: the atmosphere's tables are built once, a few hundred milliseconds.

#include <cy/rendering/fog/volume.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/tables.h>
#include <cy/test/test.h>

#include <cmath>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using namespace cy::rendering::fog;
namespace sky = cy::rendering::sky;

struct Air {
    sky::Atmosphere atmosphere = sky::earth_atmosphere();
    sky::AtmosphereTables tables;
    sky::AerialPerspectiveTable table;
    FogSettings settings;
    FogView view;
    bool built = false;

    Air() {
        settings.volume.width = 12;
        settings.volume.height = 8;
        settings.volume.depth = 24;
        settings.volume.near_plane = 1.0F;
        settings.volume.far_plane = 6000.0F;
        settings.volume.depth_exponent = 2.0F;
        settings.steps_per_slice = 2;
        view.forward = normalize(Vec3{0.0F, -0.05F, -1.0F});
        view.right = normalize(cross(view.forward, Vec3{0.0F, 1.0F, 0.0F}));
        view.up = cross(view.right, view.forward);
        view.eye = Vec3{0.0F, 120.0F, 0.0F};
        sky::AerialPerspectiveTable::View basis;
        basis.forward = view.forward;
        basis.right = view.right;
        basis.up = view.up;
        basis.tan_half_fov_x = view.tan_half_fov_x;
        basis.tan_half_fov_y = view.tan_half_fov_y;
        built = tables.configure(sky::SkyTableQuality::Medium).has_value() &&
                tables.build(atmosphere).has_value() &&
                table.configure(settings.volume).has_value() &&
                table.update(atmosphere, tables, sky::ground_position(atmosphere, 120.0F), basis,
                             normalize(Vec3{0.4F, 0.3F, -0.6F}))
                    .has_value();
    }
};

FogLight daylight() noexcept {
    FogLight light;
    light.to_sun = normalize(Vec3{0.4F, 0.3F, -0.6F});
    light.sun_illuminance = Vec3{1.0F, 0.95F, 0.85F};
    light.ambient_radiance = Vec3{0.2F, 0.25F, 0.35F};
    return light;
}

f32 relative(f32 got, f32 want) noexcept {
    return std::fabs(got - want) / std::fmax(std::fabs(want), 1e-9F);
}

}  // namespace

CY_TEST_CASE("an empty fog marched with the atmosphere's table reproduces the table") {
    const Air air;
    CY_REQUIRE(air.built);
    const cy::rendering::FroxelVolume& volume = air.settings.volume;
    std::vector<Vec3> t(volume.depth);
    std::vector<Vec3> s(volume.depth);
    f32 worst_t = 0.0F;
    f32 worst_s = 0.0F;
    for (u32 y = 0; y < volume.height; y += 3) {
        for (u32 x = 0; x < volume.width; x += 5) {
            CY_REQUIRE(integrate_fog_column(air.settings, air.view, daylight(), FogShadow{},
                                            HostShadowMap{}, FogMedium{}, x, y, cy::Span<Vec3>(t),
                                            cy::Span<Vec3>(s), &air.table)
                           .has_value());
            const Vec3 direction = fog_column_direction(air.settings, air.view, x, y);
            const f32 cosine = dot(direction, air.view.forward);
            for (u32 slice = 0; slice < volume.depth; ++slice) {
                const f32 distance = cy::rendering::froxel_slice_depth(volume, slice) / cosine;
                const sky::AerialPerspective want = air.table.sample_at(direction * distance);
                for (u32 channel = 0; channel < 3; ++channel) {
                    worst_t = std::fmax(worst_t, relative(t[slice][channel],
                                                          want.transmittance[channel]));
                    worst_s = std::fmax(worst_s, relative(s[slice][channel],
                                                          want.in_scattering[channel]));
                }
            }
        }
    }
    CY_TEST_MESSAGE("empty fog over the air: worst transmittance ", worst_t,
                    ", worst in-scattering ", worst_s);
    CY_CHECK_LT(worst_t, 1e-4F);
    CY_CHECK_LT(worst_s, 1e-3F);
}

CY_TEST_CASE("a switched-off atmosphere leaves the fog exactly as it was") {
    Air air;
    CY_REQUIRE(air.built);
    FogMedium medium;
    medium.height.extinction = extinction_for_visibility(2000.0F);
    medium.height.base_height = 0.0F;
    medium.height.scale_height = 80.0F;
    const cy::rendering::FroxelVolume& volume = air.settings.volume;
    std::vector<Vec3> alone_t(volume.depth);
    std::vector<Vec3> alone_s(volume.depth);
    std::vector<Vec3> with_t(volume.depth);
    std::vector<Vec3> with_s(volume.depth);
    sky::AerialPerspectiveTable off;
    CY_REQUIRE(off.configure(volume).has_value());
    for (const u32 column : {0U, 7U, 11U}) {
        CY_REQUIRE(integrate_fog_column(air.settings, air.view, daylight(), FogShadow{},
                                        HostShadowMap{}, medium, column, 4, cy::Span<Vec3>(alone_t),
                                        cy::Span<Vec3>(alone_s))
                       .has_value());
        CY_REQUIRE(integrate_fog_column(air.settings, air.view, daylight(), FogShadow{},
                                        HostShadowMap{}, medium, column, 4, cy::Span<Vec3>(with_t),
                                        cy::Span<Vec3>(with_s), &off)
                       .has_value());
        for (u32 slice = 0; slice < volume.depth; ++slice) {
            CY_CHECK(with_t[slice] == alone_t[slice]);
            CY_CHECK(with_s[slice] == alone_s[slice]);
        }
    }
}

CY_TEST_CASE("fog and air together transmit the product and add less than the sum") {
    const Air air;
    CY_REQUIRE(air.built);
    FogMedium medium;
    medium.height.extinction = extinction_for_visibility(1500.0F);
    medium.height.base_height = 1000.0F;  // homogeneous over the camera's altitude
    const cy::rendering::FroxelVolume& volume = air.settings.volume;
    std::vector<Vec3> fog_t(volume.depth);
    std::vector<Vec3> fog_s(volume.depth);
    std::vector<Vec3> air_t(volume.depth);
    std::vector<Vec3> air_s(volume.depth);
    std::vector<Vec3> both_t(volume.depth);
    std::vector<Vec3> both_s(volume.depth);
    for (const u32 column : {1U, 6U, 10U}) {
        CY_REQUIRE(integrate_fog_column(air.settings, air.view, daylight(), FogShadow{},
                                        HostShadowMap{}, medium, column, 3, cy::Span<Vec3>(fog_t),
                                        cy::Span<Vec3>(fog_s))
                       .has_value());
        CY_REQUIRE(integrate_fog_column(air.settings, air.view, daylight(), FogShadow{},
                                        HostShadowMap{}, FogMedium{}, column, 3,
                                        cy::Span<Vec3>(air_t), cy::Span<Vec3>(air_s), &air.table)
                       .has_value());
        CY_REQUIRE(integrate_fog_column(air.settings, air.view, daylight(), FogShadow{},
                                        HostShadowMap{}, medium, column, 3, cy::Span<Vec3>(both_t),
                                        cy::Span<Vec3>(both_s), &air.table)
                       .has_value());
        for (u32 slice = 0; slice < volume.depth; ++slice) {
            for (u32 channel = 0; channel < 3; ++channel) {
                CY_CHECK_LT(relative(both_t[slice][channel],
                                     fog_t[slice][channel] * air_t[slice][channel]),
                            1e-4F);
                CY_CHECK_LE(both_s[slice][channel],
                            (fog_s[slice][channel] + air_s[slice][channel]) * 1.0001F);
                CY_CHECK_GE(both_s[slice][channel],
                            std::fmax(fog_s[slice][channel] * air_t[slice][channel],
                                      air_s[slice][channel] * fog_t[slice][channel]) *
                                0.9999F);
            }
        }
        // Far enough, and the fog has hidden most of what the air behind it would have added.
        const u32 last = volume.depth - 1U;
        CY_CHECK_LT(both_s[last].z, (fog_s[last].z + air_s[last].z) * 0.99F);
    }
}
