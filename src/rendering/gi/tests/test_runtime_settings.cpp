// SPDX-License-Identifier: MIT
// The settings a game or an editor changes while the world runs: the mode, the global quality and
// tier, hardware ray tracing and the budget — `IlluminationSystem::set_runtime_settings`, which the
// Swift API (ABI 1.9's `gi_set_settings`) and the editor's per-world GI settings reach.
//
// `rendering-global-illumination` — "GI volumes" (the global settings a volume overrides) and
// "Hardware ray tracing with a software fallback" (the profile switch). Each case holds a setting
// to what it changes, and the untouched settings to the configured system bit for bit.

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/gi_volume.h>
#include <cy/rendering/gi/system.h>

#include "support.h"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

using cy::Aabb;
using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::rendering::gi::FrameContext;
using cy::rendering::gi::GiLight;
using cy::rendering::gi::GiMode;
using cy::rendering::gi::GiQuality;
using cy::rendering::gi::GiRuntimeSettings;
using cy::rendering::gi::GiTierPreference;
using cy::rendering::gi::GiVolume;
using cy::rendering::gi::GiVolumeParameter;
using cy::rendering::gi::gi_volume_bit;
using cy::rendering::gi::IlluminationSettings;
using cy::rendering::gi::IlluminationSystem;
using cy::rendering::gi::Probe;
using cy::rendering::gi::RadianceSource;
using cy::rendering::gi::SurfaceProperties;

const Vec3 kQuery{-2.0F, -1.5F, -2.0F};
const Vec3 kUp{0.0F, 1.0F, 0.0F};

/// support.h's room with both tiers available: the field, the cards and the triangles.
struct Room {
    gi_support::RoomField field{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    gi_support::RoomTriangles triangles{false};
    std::vector<cy::rendering::gi::Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<GiLight> lights = gi_support::room_lights();
    IlluminationSystem system;

    Room() {
        IlluminationSettings settings = gi_support::room_settings();
        settings.ray_tracing.device_supports_ray_tracing = true;
        settings.ray_tracing.budget.max_builds_per_frame = 8;
        CY_REQUIRE(system.configure(settings).has_value());
        CY_REQUIRE(system.field().place(1, field.asset(), cy::Mat4::identity()).has_value());
        const Aabb bounds = Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
        CY_REQUIRE(system.ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        system.surfaces().set_lookup_radius(1.2F);
        CY_REQUIRE(
            system.acceleration()
                .declare_geometry(1, cy::rendering::rt::adapt_static_mesh(triangles.geometry(), 0))
                .has_value());
        cy::rendering::rt::InstanceDescriptor instance;
        instance.geometry = 1;
        instance.instance_id = 1;
        CY_REQUIRE(system.acceleration().add_instance(instance).has_value());
    }

    [[nodiscard]] FrameContext context(u64 frame) const noexcept {
        FrameContext out;
        out.lights = {lights.data(), lights.size()};
        out.frame = frame;
        out.measured_gi_ms = 1.0F;
        return out;
    }

    void converge(u64 first = 1) { (void)system.advance_to_convergence(context(first), 0.95F, 8); }

    [[nodiscard]] cy::rendering::gi::TracingDiagnostics specular_rays(Vec3 at) {
        system.tracer().reset_diagnostics();
        cy::rendering::gi::TraceBudget budget;
        const Vec3 view = cy::normalize(Vec3{1.0F, -1.0F, 0.3F});
        (void)system.indirect_specular(at, kUp, view, 0.2F, 3, budget);
        return system.tracer().diagnostics();
    }
};

void check_same(Vec3 a, Vec3 b) {
    CY_CHECK_EQ(a.x, b.x);
    CY_CHECK_EQ(a.y, b.y);
    CY_CHECK_EQ(a.z, b.z);
}

}  // namespace

CY_TEST_CASE("the run-time settings start as the configured ones, and applying them is a no-op") {
    Room configured;
    Room applied;
    const GiRuntimeSettings start = applied.system.runtime_settings();
    CY_CHECK_EQ(start.mode, GiMode::Dynamic);
    CY_CHECK_EQ(start.quality, GiQuality::Medium);
    CY_CHECK_EQ(start.tier, GiTierPreference::Automatic);
    CY_CHECK(start.hardware_ray_tracing);
    CY_CHECK_EQ(start.budget_ms, IlluminationSettings{}.budget.allocation_ms);
    CY_REQUIRE(applied.system.set_runtime_settings(start).has_value());

    configured.converge();
    applied.converge();
    CY_CHECK_EQ(configured.system.radiance().probe_count(), applied.system.radiance().probe_count());
    for (const auto& [point, normal] :
         {std::pair{kQuery, kUp}, std::pair{Vec3{2.0F, 0.0F, -3.9F}, Vec3{0.0F, 0.0F, 1.0F}}}) {
        const Vec3 a = configured.system.indirect_diffuse(point, normal, SurfaceProperties{}).radiance;
        const Vec3 b = applied.system.indirect_diffuse(point, normal, SurfaceProperties{}).radiance;
        CY_CHECK_GT(a.y, 0.0F);
        check_same(a, b);
    }
}

CY_TEST_CASE("a global quality places every probe again at its ray scale, under any volume") {
    Room room;
    room.converge();
    GiRuntimeSettings settings = room.system.runtime_settings();
    settings.quality = GiQuality::High;
    CY_REQUIRE(room.system.set_runtime_settings(settings).has_value());
    CY_CHECK_EQ(room.system.runtime_settings().quality, GiQuality::High);
    // Every probe was retired: the next frame places them all again, at twice the rays.
    CY_CHECK_EQ(room.system.radiance().probe_count(), 0U);
    FrameContext all = room.context(20);
    all.converged_mode = true;
    const auto report = room.system.update(all);
    CY_REQUIRE(report.probes.probes_updated > 0U);
    for (const Probe& probe : room.system.radiance().probes()) {
        if (probe.live) {
            CY_CHECK_EQ(probe.ray_scale, 2.0F);
        }
    }
    // Twice the rays per probe of the same frame at the configured quality.
    Room control;
    control.converge();
    const auto medium = control.system.update(all);
    CY_REQUIRE(medium.probes.probes_updated > 0U);
    CY_CHECK_EQ(report.probes.rays * medium.probes.probes_updated,
                2U * medium.probes.rays * report.probes.probes_updated);

    // A volume still has the last word where it reaches.
    GiVolume low;
    low.bounds = Aabb::from_center_extents(kQuery, Vec3{1.5F, 1.5F, 1.5F});
    low.blend_distance_metres = 0.0F;
    low.sets = gi_volume_bit(GiVolumeParameter::Quality);
    low.quality = GiQuality::Low;
    CY_REQUIRE(room.system.set_volumes({&low, 1}).has_value());
    (void)room.system.update(room.context(21));
    u32 inside = 0;
    for (const Probe& probe : room.system.radiance().probes()) {
        if (!probe.live) {
            continue;
        }
        const bool in_volume = low.bounds.contains(probe.position);
        inside += in_volume ? 1U : 0U;
        CY_CHECK_EQ(probe.ray_scale, in_volume ? 0.5F : 2.0F);
    }
    CY_CHECK_GT(inside, 0U);
    CY_CHECK_EQ(room.system.region_settings(Vec3{3.0F, 0.0F, 3.0F}).ray_scale, 2.0F);
}

CY_TEST_CASE("a global software-only tier keeps every ray off the hardware tier") {
    Room room;
    room.converge();
    CY_REQUIRE_EQ(room.system.tracer().selected_world_tier(), RadianceSource::HardwareTrace);
    const auto before = room.specular_rays(kQuery);
    CY_REQUIRE(before.hardware_rays > 0U);

    GiRuntimeSettings settings = room.system.runtime_settings();
    settings.tier = GiTierPreference::SoftwareOnly;
    CY_REQUIRE(room.system.set_runtime_settings(settings).has_value());
    const auto after = room.specular_rays(kQuery);
    CY_CHECK_EQ(after.hardware_rays, 0U);
    CY_CHECK_GT(after.software_rays, 0U);
    (void)room.system.update(room.context(30));
    for (const Probe& probe : room.system.radiance().probes()) {
        if (probe.live) {
            CY_CHECK_EQ(probe.tier, GiTierPreference::SoftwareOnly);
        }
    }
}

CY_TEST_CASE("hardware ray tracing off is the software tier, and on again is the hardware tier") {
    Room room;
    room.converge();
    CY_REQUIRE_EQ(room.system.tracer().selected_world_tier(), RadianceSource::HardwareTrace);
    const u32 probes = room.system.radiance().probe_count();

    GiRuntimeSettings settings = room.system.runtime_settings();
    settings.hardware_ray_tracing = false;
    CY_REQUIRE(room.system.set_runtime_settings(settings).has_value());
    CY_CHECK_FALSE(room.system.runtime_settings().hardware_ray_tracing);
    CY_CHECK_FALSE(room.system.acceleration().active());
    CY_CHECK_EQ(room.system.tracer().selected_world_tier(), RadianceSource::SoftwareTrace);
    const auto off = room.specular_rays(kQuery);
    CY_CHECK_EQ(off.hardware_rays, 0U);
    // The tier is decided per ray, not per probe: no probe was retired.
    CY_CHECK_EQ(room.system.radiance().probe_count(), probes);

    settings.hardware_ray_tracing = true;
    CY_REQUIRE(room.system.set_runtime_settings(settings).has_value());
    (void)room.system.update(room.context(40));
    CY_CHECK(room.system.acceleration().active());
    CY_CHECK_EQ(room.system.tracer().selected_world_tier(), RadianceSource::HardwareTrace);
    CY_CHECK_GT(room.specular_rays(kQuery).hardware_rays, 0U);
}

CY_TEST_CASE("the budget is the allocation the next frame holds to, and the mode is the frame's") {
    Room room;
    room.converge();
    GiRuntimeSettings settings = room.system.runtime_settings();
    settings.budget_ms = 1.25F;
    settings.mode = GiMode::Probe;
    CY_REQUIRE(room.system.set_runtime_settings(settings).has_value());
    CY_CHECK_EQ(room.system.budget().settings().allocation_ms, 1.25F);
    CY_CHECK_EQ(room.system.settings().budget.allocation_ms, 1.25F);
    CY_CHECK_EQ(room.system.mode(), GiMode::Probe);
    const auto report = room.system.update(room.context(50));
    CY_CHECK_EQ(report.budget.allocation_ms, 1.25F);
    // `Probe` keeps the seeded caches and updates neither.
    CY_CHECK_EQ(report.probes.probes_updated, 0U);
    CY_CHECK_EQ(report.surfaces.pages_updated, 0U);
}

CY_TEST_CASE("a refused run-time setting changes nothing") {
    Room room;
    const GiRuntimeSettings start = room.system.runtime_settings();
    GiRuntimeSettings bad = start;
    bad.budget_ms = 0.0F;
    CY_CHECK_FALSE(room.system.set_runtime_settings(bad).has_value());
    bad.budget_ms = std::numeric_limits<f32>::quiet_NaN();
    CY_CHECK_FALSE(room.system.set_runtime_settings(bad).has_value());
    bad = start;
    bad.quality = GiQuality::Count;
    CY_CHECK_FALSE(room.system.set_runtime_settings(bad).has_value());
    bad = start;
    bad.mode = GiMode::Count;
    bad.hardware_ray_tracing = false;
    CY_CHECK_FALSE(room.system.set_runtime_settings(bad).has_value());
    const GiRuntimeSettings after = room.system.runtime_settings();
    CY_CHECK_EQ(after.mode, start.mode);
    CY_CHECK_EQ(after.quality, start.quality);
    CY_CHECK_EQ(after.budget_ms, start.budget_ms);
    CY_CHECK(after.hardware_ray_tracing);
}
