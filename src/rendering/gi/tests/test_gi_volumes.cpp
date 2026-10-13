// SPDX-License-Identifier: MIT
// GI volumes: how a sample resolves the volumes over it. Arithmetic only — the composed system's
// use of the result is `render_gi_volumes`.
//
// `rendering-global-illumination` — "GI volumes": overrides of quality, probe density, update
// rate, indirect diffuse and specular intensity, tier preference and far-field distance, with
// priority and blend distance between overlaps, and a scene with none lit by the global settings.

#include <cy/test/test.h>

#include <cy/rendering/gi/gi_volume.h>

#include <cmath>
#include <cstring>
#include <numbers>

namespace {

using cy::Aabb;
using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::gi::gi_volume_bit;
using cy::rendering::gi::GiQuality;
using cy::rendering::gi::GiRegionSettings;
using cy::rendering::gi::GiTierPreference;
using cy::rendering::gi::GiVolume;
using cy::rendering::gi::GiVolumeParameter;
using cy::rendering::gi::GiVolumeSet;

/// A 4 m cube at the origin with a 2 m blend band, setting `parameters`.
GiVolume cube(u32 parameters, cy::i32 priority = 0) noexcept {
    GiVolume volume;
    volume.bounds = Aabb::from_min_max(Vec3{-2.0F, -2.0F, -2.0F}, Vec3{2.0F, 2.0F, 2.0F});
    volume.blend_distance_metres = 2.0F;
    volume.priority = priority;
    volume.sets = parameters;
    return volume;
}

GiRegionSettings global() noexcept {
    GiRegionSettings settings;
    settings.far_field_begins_metres = 60.0F;
    return settings;
}

/// Field by field, each compared as bits: the struct has padding, which memcmp would compare too.
bool same_bits(const GiRegionSettings& a, const GiRegionSettings& b) noexcept {
    const auto bits = [](f32 value) {
        u32 word = 0;
        std::memcpy(&word, &value, sizeof(word));
        return word;
    };
    return bits(a.ray_scale) == bits(b.ray_scale) &&
           bits(a.probe_density) == bits(b.probe_density) &&
           bits(a.update_rate) == bits(b.update_rate) &&
           bits(a.diffuse_intensity) == bits(b.diffuse_intensity) &&
           bits(a.specular_intensity) == bits(b.specular_intensity) && a.tier == b.tier &&
           bits(a.far_field_begins_metres) == bits(b.far_field_begins_metres);
}

}  // namespace

CY_TEST_CASE("with no GI volume a sample resolves to the global settings bit for bit") {
    const GiVolumeSet none;
    CY_CHECK(none.empty());
    const GiRegionSettings settings = none.resolve(Vec3{0.0F, 0.0F, 0.0F}, global());
    CY_CHECK(same_bits(settings, global()));

    // A volume that does not reach the sample is the same as none.
    GiVolumeSet far;
    GiVolume volume = cube(gi_volume_bit(GiVolumeParameter::ProbeDensity));
    volume.probe_density = 4.0F;
    CY_REQUIRE(far.assign({&volume, 1}).has_value());
    CY_CHECK(same_bits(far.resolve(Vec3{4.5F, 0.0F, 0.0F}, global()), global()));
}

CY_TEST_CASE("inside a GI volume every parameter it sets is overridden, and only those") {
    GiVolume volume = cube(gi_volume_bit(GiVolumeParameter::Quality) |
                           gi_volume_bit(GiVolumeParameter::ProbeDensity) |
                           gi_volume_bit(GiVolumeParameter::UpdateRate) |
                           gi_volume_bit(GiVolumeParameter::DiffuseIntensity) |
                           gi_volume_bit(GiVolumeParameter::SpecularIntensity) |
                           gi_volume_bit(GiVolumeParameter::TierPreference) |
                           gi_volume_bit(GiVolumeParameter::FarFieldDistance));
    volume.quality = GiQuality::Epic;
    volume.probe_density = 2.0F;
    volume.update_rate = 3.0F;
    volume.diffuse_intensity = 0.5F;
    volume.specular_intensity = 1.5F;
    volume.tier = GiTierPreference::SoftwareOnly;
    volume.far_field_begins_metres = 12.0F;
    GiVolumeSet set;
    CY_REQUIRE(set.assign({&volume, 1}).has_value());

    const GiRegionSettings inside = set.resolve(Vec3{1.0F, 0.0F, 0.0F}, global());
    CY_CHECK_EQ(inside.ray_scale, 4.0F);
    CY_CHECK_EQ(inside.probe_density, 2.0F);
    CY_CHECK_EQ(inside.update_rate, 3.0F);
    CY_CHECK_EQ(inside.diffuse_intensity, 0.5F);
    CY_CHECK_EQ(inside.specular_intensity, 1.5F);
    CY_CHECK_EQ(inside.tier, GiTierPreference::SoftwareOnly);
    CY_CHECK_EQ(inside.far_field_begins_metres, 12.0F);

    // Per parameter: a volume that sets only the update rate leaves the density where it was.
    GiVolume rate_only = cube(gi_volume_bit(GiVolumeParameter::UpdateRate));
    rate_only.update_rate = 2.0F;
    rate_only.probe_density = 8.0F;  // declared, not set: ignored
    CY_REQUIRE(set.assign({&rate_only, 1}).has_value());
    const GiRegionSettings partial = set.resolve(Vec3{0.0F, 0.0F, 0.0F}, global());
    CY_CHECK_EQ(partial.update_rate, 2.0F);
    CY_CHECK_EQ(partial.probe_density, 1.0F);
}

CY_TEST_CASE(
    "a GI volume fades in over its blend distance, and a discrete override switches halfway") {
    GiVolume volume = cube(gi_volume_bit(GiVolumeParameter::DiffuseIntensity) |
                           gi_volume_bit(GiVolumeParameter::TierPreference));
    volume.diffuse_intensity = 3.0F;
    volume.tier = GiTierPreference::WorldOnly;
    GiVolumeSet set;
    CY_REQUIRE(set.assign({&volume, 1}).has_value());

    // At the boundary, a quarter of the way through the band, at three quarters, and beyond it.
    CY_CHECK_EQ(set.resolve(Vec3{2.0F, 0.0F, 0.0F}, global()).diffuse_intensity, 3.0F);
    const GiRegionSettings quarter = set.resolve(Vec3{2.5F, 0.0F, 0.0F}, global());
    CY_CHECK_NEAR(quarter.diffuse_intensity, 1.0F + (2.0F * 0.75F), 1.0e-6F);
    CY_CHECK_EQ(quarter.tier, GiTierPreference::WorldOnly);
    const GiRegionSettings outer = set.resolve(Vec3{3.5F, 0.0F, 0.0F}, global());
    CY_CHECK_NEAR(outer.diffuse_intensity, 1.0F + (2.0F * 0.25F), 1.0e-6F);
    CY_CHECK_EQ(outer.tier, GiTierPreference::Automatic);
    CY_CHECK_EQ(set.resolve(Vec3{4.0F, 0.0F, 0.0F}, global()).diffuse_intensity, 1.0F);

    // The distance is to the box, not its centre: diagonally out from a corner.
    const f32 corner = set.resolve(Vec3{3.0F, 3.0F, 2.0F}, global()).diffuse_intensity;
    const f32 fade = 1.0F - (std::numbers::sqrt2_v<f32> / 2.0F);
    CY_CHECK_NEAR(corner, 1.0F + (2.0F * fade), 1.0e-6F);

    // A master weight dials it down without moving it; zero blend distance switches at the edge.
    volume.weight = 0.5F;
    volume.blend_distance_metres = 0.0F;
    CY_REQUIRE(set.assign({&volume, 1}).has_value());
    CY_CHECK_EQ(set.resolve(Vec3{0.0F, 0.0F, 0.0F}, global()).diffuse_intensity, 2.0F);
    CY_CHECK_EQ(set.resolve(Vec3{2.001F, 0.0F, 0.0F}, global()).diffuse_intensity, 1.0F);
}

CY_TEST_CASE("where GI volumes overlap the higher priority dominates, whatever the order given") {
    GiVolume low = cube(gi_volume_bit(GiVolumeParameter::ProbeDensity), 0);
    low.probe_density = 0.5F;
    GiVolume high = cube(gi_volume_bit(GiVolumeParameter::ProbeDensity), 5);
    high.probe_density = 4.0F;
    high.bounds = Aabb::from_min_max(Vec3{0.0F, -2.0F, -2.0F}, Vec3{2.0F, 2.0F, 2.0F});
    high.blend_distance_metres = 1.0F;

    for (const bool high_first : {false, true}) {
        const GiVolume ordered[2] = {high_first ? high : low, high_first ? low : high};
        GiVolumeSet set;
        CY_REQUIRE(set.assign({ordered, 2}).has_value());
        CY_TEST_INFO("high first " << high_first);
        // In both: the high-priority volume replaces the low one entirely.
        CY_CHECK_EQ(set.resolve(Vec3{1.0F, 0.0F, 0.0F}, global()).probe_density, 4.0F);
        // In the low one only: its own value.
        CY_CHECK_EQ(set.resolve(Vec3{-1.5F, 0.0F, 0.0F}, global()).probe_density, 0.5F);
        // In the low one and half way into the high one's band: the high one blends over the low.
        CY_CHECK_NEAR(set.resolve(Vec3{-0.5F, 0.0F, 0.0F}, global()).probe_density,
                      0.5F + ((4.0F - 0.5F) * 0.5F), 1.0e-6F);
    }

    // Equal priorities apply in the order given, the later one last.
    GiVolume first = cube(gi_volume_bit(GiVolumeParameter::UpdateRate), 1);
    first.update_rate = 2.0F;
    GiVolume second = first;
    second.update_rate = 6.0F;
    const GiVolume pair[2] = {first, second};
    GiVolumeSet set;
    CY_REQUIRE(set.assign({pair, 2}).has_value());
    CY_CHECK_EQ(set.resolve(Vec3{0.0F, 0.0F, 0.0F}, global()).update_rate, 6.0F);
}

CY_TEST_CASE(
    "a GI volume whose numbers cannot mean anything is refused, and the set is unchanged") {
    GiVolume valid = cube(gi_volume_bit(GiVolumeParameter::UpdateRate));
    valid.update_rate = 2.0F;
    GiVolumeSet set;
    CY_REQUIRE(set.assign({&valid, 1}).has_value());
    const cy::u64 revision = set.revision();

    const auto refused = [&](GiVolume broken) {
        const GiVolume both[2] = {valid, broken};
        return !set.assign({both, 2}).has_value();
    };
    GiVolume broken = valid;
    broken.bounds = Aabb{};
    CY_CHECK(refused(broken));
    broken = valid;
    broken.blend_distance_metres = -1.0F;
    CY_CHECK(refused(broken));
    broken = valid;
    broken.weight = 1.5F;
    CY_CHECK(refused(broken));
    broken = valid;
    broken.probe_density = 0.0F;
    CY_CHECK(refused(broken));
    broken = valid;
    broken.update_rate = -2.0F;
    CY_CHECK(refused(broken));
    broken = valid;
    broken.diffuse_intensity = -0.1F;
    CY_CHECK(refused(broken));
    broken = valid;
    broken.far_field_begins_metres = 0.0F;
    CY_CHECK(refused(broken));
    broken = valid;
    broken.sets = 1U << 7U;
    CY_CHECK(refused(broken));
    broken = valid;
    broken.tier = GiTierPreference::Count;
    CY_CHECK(refused(broken));

    CY_CHECK_EQ(set.revision(), revision);
    CY_CHECK_EQ(set.size(), 1U);
    CY_CHECK_EQ(set.resolve(Vec3{0.0F, 0.0F, 0.0F}, global()).update_rate, 2.0F);
}
