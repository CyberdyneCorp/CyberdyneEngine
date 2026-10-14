// SPDX-License-Identifier: MIT
// Transparency and participating media, through the composed system.
//
// `rendering-global-illumination` — "Transparency and participating media": the staging table as
// code, a backlit thin-transmission surface measured against the path tracer, the low-frequency
// indirect a participating medium takes from the radiance cache measured against the path tracer,
// and the two deferred features refused by name.

#include <cy/test/test.h>

#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/media.h>
#include <cy/rendering/gi/system.h>

#include "rooms.h"
#include "support.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

/// Bit-for-bit equality of two vectors, member by member.
[[nodiscard]] bool same_bits(cy::Vec3 a, cy::Vec3 b) noexcept {
    return std::bit_cast<cy::u32>(a.x) == std::bit_cast<cy::u32>(b.x) &&
           std::bit_cast<cy::u32>(a.y) == std::bit_cast<cy::u32>(b.y) &&
           std::bit_cast<cy::u32>(a.z) == std::bit_cast<cy::u32>(b.z);
}

using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::u32;
using cy::usize;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

class AnyRefraction final : public RefractionSolver {
public:
    [[nodiscard]] RadianceSample refracted(Vec3, Vec3, Vec3, f32) const noexcept override {
        return RadianceSample{};
    }
};

class AnyCaustics final : public CausticsSource {
public:
    [[nodiscard]] Vec3 caustic(Vec3, Vec3) const noexcept override { return Vec3{}; }
};

[[nodiscard]] BakeSettings reference_settings() noexcept {
    BakeSettings settings;
    settings.bounces = 3;
    settings.samples = 2048;
    return settings;
}

}  // namespace

CY_TEST_CASE("the staging table is code, and the deferred features are refused by name") {
    CY_CHECK(transmission_support(TransmissionClass::Opaque) == TransmissionSupport::Full);
    CY_CHECK(transmission_support(TransmissionClass::Masked) == TransmissionSupport::Full);
    CY_CHECK(transmission_support(TransmissionClass::ThinTransmission) ==
             TransmissionSupport::Approximate);
    CY_CHECK(transmission_support(TransmissionClass::VolumetricMedia) ==
             TransmissionSupport::LowFrequency);
    CY_CHECK(transmission_support(TransmissionClass::Refraction) == TransmissionSupport::Deferred);
    CY_CHECK(transmission_support(TransmissionClass::Caustics) == TransmissionSupport::Deferred);

    gi_rooms::LitRoom room;
    AnyRefraction refraction;
    AnyCaustics caustics;
    const cy::Status refused_refraction = room.system.set_refraction_solver(&refraction);
    const cy::Status refused_caustics = room.system.set_caustics_source(&caustics);
    CY_REQUIRE_FALSE(refused_refraction.has_value());
    CY_REQUIRE_FALSE(refused_caustics.has_value());
    CY_CHECK(refused_refraction.error().code == cy::ErrorCode::NotImplemented);
    CY_CHECK(refused_caustics.error().code == cy::ErrorCode::NotImplemented);
    CY_CHECK(std::strstr(refused_refraction.error().message, "DEFERRED") != nullptr);
    CY_CHECK(std::strstr(refused_caustics.error().message, "caustics are DEFERRED") != nullptr);
}

CY_TEST_CASE("a backlit thin-transmission surface transmits the light its back face receives") {
    // A leaf across the room, its front to the half the light is not in and its back to the light.
    // What comes through is T times the back face's irradiance: measured through the radiance
    // cache, referenced by the path tracer's irradiance about the reversed normal at the same
    // point.
    gi_rooms::LitRoom room;
    (void)room.converge();
    const Vec3 front{0.0F, 0.0F, 1.0F};
    const Vec3 tint{0.45F, 0.6F, 0.2F};
    PathTracer tracer(room.system.world_tracer(), room.system.scene(),
                      {room.lights.data(), room.lights.size()}, room.system.sky_term(),
                      &room.system.world_tracer());
    std::vector<Vec3> measured;
    std::vector<Vec3> reference;
    for (u32 ix = 0; ix < 4; ++ix) {
        for (u32 iy = 0; iy < 3; ++iy) {
            const Vec3 leaf{-2.25F + (1.5F * static_cast<f32>(ix)), -1.0F + static_cast<f32>(iy),
                            -0.5F};
            SurfaceProperties opaque;
            SurfaceProperties thin;
            thin.transmission = tint;
            const ResolveResult solid = room.system.indirect_diffuse(leaf, front, opaque);
            const ResolveResult through = room.system.indirect_diffuse(leaf, front, thin);
            // The opaque answer is untouched, and an opaque surface transmits nothing.
            CY_CHECK(same_bits(solid.radiance, through.radiance));
            CY_CHECK_EQ(solid.transmitted.x, 0.0F);
            CY_CHECK_EQ(solid.transmitted.y, 0.0F);
            CY_CHECK_EQ(solid.transmitted.z, 0.0F);
            measured.push_back(through.transmitted);
            const Vec3 back = front * -1.0F;
            const Vec3 arriving = tracer.irradiance(
                leaf + (back * kThinTransmissionBackOffsetMetres), back, reference_settings());
            reference.push_back(cy::cwise_mul(arriving, tint));
        }
    }
    const ReferenceComparison comparison = compare_against_reference(
        {measured.data(), measured.size()}, {reference.data(), reference.size()});
    std::printf(
        "thin transmission: %u points, mean relative error %.4f against the path tracer "
        "(mean reference %.4f)\n",
        comparison.samples, static_cast<double>(comparison.relative_error),
        static_cast<double>(comparison.mean_reference_magnitude));
    CY_CHECK_GT(comparison.mean_reference_magnitude, 0.0F);
    CY_CHECK_LT(comparison.relative_error, 0.15F);
}

CY_TEST_CASE("a participating medium takes the radiance cache's low-frequency indirect") {
    // A fog filling the room, lit only by bounce: the grid the medium reads is the cache's mean
    // radiance over the sphere, referenced by the path tracer's mean over the six cardinal
    // hemispheres at the same points — the same functional, estimated independently. Probes a
    // metre apart: the medium's indirect is as fine as the cache it reads, and at the GI suites'
    // two metres the cache's interpolation is the error, not the medium.
    IlluminationSettings fine = gi_support::room_settings();
    fine.probes.base_spacing_metres = 1.0F;
    fine.probes.half_extent_probes = 4;
    gi_rooms::LitRoom room(false, fine);
    (void)room.converge();
    MediaIndirectGrid grid;
    const Aabb inside = Aabb::from_min_max(Vec3{-3.0F, -1.0F, -3.0F}, Vec3{3.0F, 1.0F, 3.0F});
    const Vec3 fallback{0.0F, 0.0F, 0.0F};
    CY_REQUIRE(room.system.media_indirect(inside, 2.0F, fallback, grid).has_value());
    CY_REQUIRE_EQ(grid.dimensions[0], 4U);
    CY_REQUIRE_EQ(grid.dimensions[1], 2U);
    CY_REQUIRE_EQ(grid.dimensions[2], 4U);

    PathTracer tracer(room.system.world_tracer(), room.system.scene(),
                      {room.lights.data(), room.lights.size()}, room.system.sky_term(),
                      &room.system.world_tracer());
    BakeSettings settings = reference_settings();
    settings.samples = 384;
    const Vec3 axes[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                          Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
    std::vector<Vec3> measured;
    std::vector<Vec3> reference;
    f32 lit_half = 0.0F;
    f32 dark_half = 0.0F;
    for (u32 z = 0; z < grid.dimensions[2]; ++z) {
        for (u32 y = 0; y < grid.dimensions[1]; ++y) {
            for (u32 x = 0; x < grid.dimensions[0]; ++x) {
                const usize index =
                    (((static_cast<usize>(z) * grid.dimensions[1]) + y) * grid.dimensions[0]) + x;
                const Vec3 point = grid.origin + (Vec3{static_cast<f32>(x), static_cast<f32>(y),
                                                       static_cast<f32>(z)} *
                                                  grid.spacing);
                CY_CHECK_GT(grid.coverage[index], 0.5F);
                Vec3 mean{0.0F, 0.0F, 0.0F};
                for (const Vec3 axis : axes) {
                    mean = mean + tracer.irradiance(point, axis, settings);
                }
                measured.push_back(grid.radiance[index]);
                reference.push_back(mean / 6.0F);
                (point.z < 0.0F ? lit_half : dark_half) += cy::length(grid.radiance[index]);
            }
        }
    }
    const ReferenceComparison comparison = compare_against_reference(
        {measured.data(), measured.size()}, {reference.data(), reference.size()});
    std::printf(
        "media indirect: %u grid points, mean relative error %.4f against the path tracer "
        "(mean reference %.4f); the lit half holds %.3f, the far half %.3f\n",
        comparison.samples, static_cast<double>(comparison.relative_error),
        static_cast<double>(comparison.mean_reference_magnitude), static_cast<double>(lit_half),
        static_cast<double>(dark_half));
    // Measured 0.125 on this room (Linux, GCC and clang alike): the cache's own spatial
    // resolution, a metre between probes, against a light two metres from the nearest points.
    CY_CHECK_LT(comparison.relative_error, 0.18F);
    // Indirectly lit is not uniformly lit: the half the light is in scatters more.
    CY_CHECK_GT(lit_half, dark_half);
}

CY_TEST_CASE("a medium where no probe reaches takes the fallback, and a bad grid is refused") {
    gi_rooms::LitRoom room;
    (void)room.converge();
    MediaIndirectGrid grid;
    const Vec3 fallback{0.2F, 0.3F, 0.4F};
    // Two hundred metres away: no probe of the room's clipmap reaches.
    const Aabb far = Aabb::from_min_max(Vec3{200.0F, 0.0F, 0.0F}, Vec3{202.0F, 0.0F, 0.0F});
    CY_REQUIRE(room.system.media_indirect(far, 1.0F, fallback, grid).has_value());
    for (usize index = 0; index < grid.radiance.size(); ++index) {
        CY_CHECK_EQ(grid.coverage[index], 0.0F);
        CY_CHECK(same_bits(grid.radiance[index], fallback));
    }
    CY_CHECK_FALSE(room.system.media_indirect(Aabb{}, 1.0F, fallback, grid).has_value());
    CY_CHECK_FALSE(room.system.media_indirect(far, 0.0F, fallback, grid).has_value());
    CY_CHECK_FALSE(build_media_indirect(room.system.radiance(),
                                        Aabb::from_min_max(Vec3{}, Vec3{100.0F, 100.0F, 100.0F}),
                                        0.5F, fallback, grid, 4096)
                       .has_value());
}
