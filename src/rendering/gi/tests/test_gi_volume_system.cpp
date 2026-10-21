// SPDX-License-Identifier: MIT
// GI volumes through the composed system: what an override does to the probes placed, the
// scheduler, the resolve and the tracer, against a twin with no volume.
//
// `rendering-global-illumination` — "GI volumes", its scenario "Important interior" (probe density
// and update rate increase there within the GI allocation) and "No volumes needed" (a scene with
// none is lit by the global settings).

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/gi_volume.h>
#include <cy/rendering/gi/system.h>

#include "support.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

using cy::Aabb;
using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::rendering::gi::FrameContext;
using cy::rendering::gi::gi_volume_bit;
using cy::rendering::gi::GiLight;
using cy::rendering::gi::GiQuality;
using cy::rendering::gi::GiTierPreference;
using cy::rendering::gi::GiVolume;
using cy::rendering::gi::GiVolumeParameter;
using cy::rendering::gi::IlluminationSettings;
using cy::rendering::gi::IlluminationSystem;
using cy::rendering::gi::Probe;
using cy::rendering::gi::SurfaceProperties;

// --- Two identical interiors -------------------------------------------------------------------
//
// Two equal pillars either side of the camera, mirror images about it, in an otherwise empty
// field. The probe window is symmetric about the camera too, so whatever the volume does not
// change, the two halves hold the same probes.

constexpr Vec3 kCamera{0.5F, 0.5F, 0.5F};
constexpr Vec3 kLeft{-2.5F, 0.5F, 0.5F};
constexpr Vec3 kRight{3.5F, 0.5F, 0.5F};
constexpr Vec3 kReach{2.5F, 2.5F, 2.5F};

struct Interiors {
    gi_support::BoxField pillar{Vec3{0.5F, 1.5F, 0.5F}};
    IlluminationSystem system;

    explicit Interiors(f32 far_field_begins = 60.0F, u32 half_extent_probes = 6) {
        IlluminationSettings settings;
        settings.error_targets.far_field_begins_metres = far_field_begins;
        settings.field.levels = 1;
        settings.field.resolution = 64;
        settings.field.base_extent_metres = 16.0F;
        settings.probes.levels = 2;
        settings.probes.base_spacing_metres = 1.0F;
        settings.probes.half_extent_probes = half_extent_probes;
        settings.rays_per_probe = 2;
        CY_REQUIRE(system.configure(settings).has_value());
        CY_REQUIRE(
            system.field().place(1, pillar.asset(), cy::Mat4::from_translation(kLeft)).has_value());
        CY_REQUIRE(system.field()
                       .place(2, pillar.asset(), cy::Mat4::from_translation(kRight))
                       .has_value());
    }

    [[nodiscard]] static FrameContext frame(u64 index) noexcept {
        FrameContext context;
        context.camera = kCamera;
        context.frame = index;
        return context;
    }

    /// A floor of cards under the pillars, so the resolve has a cache to read beside the sky.
    void add_floor() {
        std::vector<cy::rendering::gi::Surfel> cards;
        for (int x = -5; x <= 6; ++x) {
            for (int z = -5; z <= 6; ++z) {
                cy::rendering::gi::Surfel card;
                card.position = Vec3{static_cast<f32>(x), -1.0F, static_cast<f32>(z)};
                card.normal = Vec3{0.0F, 1.0F, 0.0F};
                cards.push_back(card);
            }
        }
        const Aabb bounds = Aabb::from_min_max(Vec3{-6.0F, -2.0F, -6.0F}, Vec3{7.0F, 3.0F, 7.0F});
        CY_REQUIRE(system.ingest_cell(1, bounds, {cards.data(), cards.size()}, 0).has_value());
        GiLight sun;
        sun.directional = true;
        sun.direction = cy::normalize(Vec3{0.3F, -1.0F, 0.2F});
        sun.intensity = 2.0F;
        sun_.assign(1, sun);
        FrameContext context = frame(1);
        context.lights = {sun_.data(), sun_.size()};
        (void)system.advance_to_convergence(context, 0.95F, 3);
    }

    std::vector<GiLight> sun_;

    /// Level 0 probes inside the region around `centre`.
    [[nodiscard]] u32 probes_around(Vec3 centre) const {
        const Aabb region = Aabb::from_center_extents(centre, kReach);
        u32 count = 0;
        for (const Probe& probe : system.radiance().probes()) {
            count += probe.live && probe.level == 0 && region.contains(probe.position) ? 1U : 0U;
        }
        return count;
    }
};

GiVolume important_interior() noexcept {
    GiVolume volume;
    volume.bounds = Aabb::from_center_extents(kRight, kReach);
    volume.blend_distance_metres = 0.0F;
    volume.sets = gi_volume_bit(GiVolumeParameter::ProbeDensity) |
                  gi_volume_bit(GiVolumeParameter::UpdateRate) |
                  gi_volume_bit(GiVolumeParameter::Quality);
    volume.probe_density = 2.0F;
    volume.update_rate = 4.0F;
    volume.quality = GiQuality::High;
    volume.id = 77;
    return volume;
}

/// The room of support.h, with both tiers available, for the cases that resolve and trace.
struct Room {
    gi_support::RoomField field{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    gi_support::RoomTriangles triangles{false};
    std::vector<cy::rendering::gi::Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<GiLight> lights = gi_support::room_lights();
    IlluminationSystem system;

    explicit Room(f32 far_field_begins = 60.0F) {
        IlluminationSettings settings = gi_support::room_settings();
        settings.error_targets.far_field_begins_metres = far_field_begins;
        settings.ray_tracing.device_supports_ray_tracing = true;
        settings.ray_tracing.budget.max_builds_per_frame = 8;
        CY_REQUIRE(system.configure(settings).has_value());
        CY_REQUIRE(system.field().place(1, field.asset(), cy::Mat4::identity()).has_value());
        const Aabb bounds = Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
        CY_REQUIRE(
            system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
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

    void converge() {
        FrameContext context;
        context.lights = {lights.data(), lights.size()};
        (void)system.advance_to_convergence(context, 0.95F, 8);
    }
};

const Vec3 kQuery{-2.0F, -1.5F, -2.0F};
const Vec3 kUp{0.0F, 1.0F, 0.0F};

}  // namespace

CY_TEST_CASE("an important interior gets more probes and more updates within one allocation") {
    Interiors control;
    Interiors marked;
    const GiVolume volume = important_interior();
    CY_REQUIRE(marked.system.set_volumes({&volume, 1}).has_value());

    (void)control.system.update(Interiors::frame(1));
    const auto placed = marked.system.update(Interiors::frame(1));

    // Without the volume the two pillars hold the same probes; with it the marked one holds more,
    // and the other is exactly what it was.
    const u32 left = control.probes_around(kLeft);
    CY_CHECK_EQ(control.probes_around(kRight), left);
    CY_CHECK_EQ(marked.probes_around(kLeft), left);
    CY_CHECK_GT(marked.probes_around(kRight), left);
    CY_CHECK_GT(placed.placement.placed_by_volume_density, 0U);
    std::printf("  probes around each pillar: %u unmarked, %u in the volume\n", left,
                marked.probes_around(kRight));

    // What the volume resolved to is on every probe it reaches, and on no other.
    for (const Probe& probe : marked.system.radiance().probes()) {
        if (!probe.live) {
            continue;
        }
        const bool inside = volume.bounds.contains(probe.position);
        CY_CHECK_EQ(probe.update_rate, inside ? 4.0F : 1.0F);
        CY_CHECK_EQ(probe.ray_scale, inside ? 2.0F : 1.0F);
    }

    // Twenty frames to place and first shade every probe (about 2 000 of them at 256 an update),
    // then forty under the same budget: every frame spends exactly what the budget allows, and a
    // probe in the volume is updated several times as often as its mirror image.
    for (u64 index = 2; index <= 21; ++index) {
        (void)marked.system.update(Interiors::frame(index));
    }
    std::vector<u32> updates(marked.system.radiance().probes().size(), 0);
    for (u64 index = 22; index <= 61; ++index) {
        const auto report = marked.system.update(Interiors::frame(index));
        const auto budget = static_cast<u32>(
            marked.system.budget().value(cy::rendering::gi::GiLever::ProbeUpdates));
        CY_CHECK_EQ(report.probes.probes_updated, std::min(budget, report.probes.queue_depth));
        const auto probes = marked.system.radiance().probes();
        u32 expected_rays = 0;
        for (u32 handle = 0; handle < probes.size(); ++handle) {
            if (probes[handle].live && probes[handle].last_update_frame == index) {
                updates[handle] += 1;
                // High quality: twice the rays per probe in the volume, the base count elsewhere.
                expected_rays += volume.bounds.contains(probes[handle].position) ? 4U : 2U;
            }
        }
        CY_CHECK_EQ(report.probes.rays, expected_rays);
    }
    const Aabb left_region = Aabb::from_center_extents(kLeft, kReach);
    u32 left_updates = 0;
    u32 left_probes = 0;
    u32 right_updates = 0;
    u32 right_probes = 0;
    const auto probes = marked.system.radiance().probes();
    for (u32 handle = 0; handle < probes.size(); ++handle) {
        if (!probes[handle].live || probes[handle].level != 0) {
            continue;
        }
        if (volume.bounds.contains(probes[handle].position)) {
            right_updates += updates[handle];
            right_probes += 1;
        } else if (left_region.contains(probes[handle].position)) {
            left_updates += updates[handle];
            left_probes += 1;
        }
    }
    const f32 right_rate = static_cast<f32>(right_updates) / static_cast<f32>(right_probes);
    const f32 left_rate = static_cast<f32>(left_updates) / static_cast<f32>(left_probes);
    std::printf("  updates per probe over 40 frames: %.2f unmarked, %.2f in the volume\n",
                static_cast<double>(left_rate), static_cast<double>(right_rate));
    CY_CHECK_GT(left_rate, 0.0F);
    CY_CHECK_GT(right_rate, 2.5F * left_rate);
}

CY_TEST_CASE("removing a GI volume places its region again at the global density") {
    Interiors control;
    Interiors marked;
    const GiVolume volume = important_interior();
    CY_REQUIRE(marked.system.set_volumes({&volume, 1}).has_value());
    (void)control.system.update(Interiors::frame(1));
    (void)marked.system.update(Interiors::frame(1));
    CY_REQUIRE(marked.probes_around(kRight) > control.probes_around(kRight));

    CY_REQUIRE(marked.system.set_volumes({}).has_value());
    CY_CHECK(marked.system.volumes().empty());
    (void)marked.system.update(Interiors::frame(2));
    CY_CHECK_EQ(marked.probes_around(kRight), control.probes_around(kRight));
    for (const Probe& probe : marked.system.radiance().probes()) {
        if (probe.live) {
            CY_CHECK_EQ(probe.update_rate, 1.0F);
            CY_CHECK_EQ(probe.ray_scale, 1.0F);
        }
    }
}

CY_TEST_CASE("each owner replaces its own GI volumes, and a volume placed again keeps its probes") {
    using cy::rendering::gi::GiVolumeOwner;
    Interiors marked;
    const GiVolume authored = important_interior();
    GiVolume game = important_interior();
    game.bounds = Aabb::from_center_extents(kLeft, kReach);
    game.id = 2;
    CY_REQUIRE(marked.system.set_volumes(GiVolumeOwner::Authored, {&authored, 1}).has_value());
    CY_REQUIRE(marked.system.set_volumes(GiVolumeOwner::Game, {&game, 1}).has_value());
    CY_CHECK_EQ(marked.system.volumes().size(), 2U);
    (void)marked.system.update(Interiors::frame(1));
    (void)marked.system.update(Interiors::frame(2));
    const auto revisions = [&marked](Vec3 centre) {
        std::vector<u32> out;
        const Aabb region = Aabb::from_center_extents(centre, kReach);
        for (const Probe& probe : marked.system.radiance().probes()) {
            if (probe.live && region.contains(probe.position)) {
                out.push_back(probe.revision);
            }
        }
        return out;
    };
    const std::vector<u32> right = revisions(kRight);
    const std::vector<u32> left = revisions(kLeft);
    CY_REQUIRE(!right.empty());
    CY_REQUIRE(!left.empty());

    // The author places the same volume again — an editor applying its document in place: no
    // probe is retired, on either side.
    CY_REQUIRE(marked.system.set_volumes({&authored, 1}).has_value());
    CY_CHECK(revisions(kRight) == right);
    CY_CHECK(revisions(kLeft) == left);
    // The author removes theirs: the game's stays placed, and its probes untouched.
    CY_REQUIRE(marked.system.set_volumes({}).has_value());
    CY_CHECK_EQ(marked.system.volumes().size(), 1U);
    CY_CHECK_EQ(marked.system.volumes_of(GiVolumeOwner::Game).size(), 1U);
    CY_CHECK(revisions(kLeft) == left);
    CY_CHECK(revisions(kRight) != right);
}

CY_TEST_CASE("a scene with no GI volume reaching it resolves exactly as one with none placed") {
    // One room with no volume, one with a volume that does not reach it: the same probes, the
    // same frame reports and the same resolve, bit for bit.
    Room none;
    Room distant;
    GiVolume far = important_interior();
    far.bounds = Aabb::from_center_extents(Vec3{200.0F, 0.0F, 0.0F}, kReach);
    far.sets |= gi_volume_bit(GiVolumeParameter::DiffuseIntensity);
    far.diffuse_intensity = 0.25F;
    CY_REQUIRE(distant.system.set_volumes({&far, 1}).has_value());
    none.converge();
    distant.converge();
    CY_CHECK_EQ(none.system.radiance().probe_count(), distant.system.radiance().probe_count());
    for (const auto& receiver :
         {std::pair{kQuery, kUp}, std::pair{Vec3{2.0F, 0.0F, -3.9F}, Vec3{0.0F, 0.0F, 1.0F}}}) {
        const Vec3 a =
            none.system.indirect_diffuse(receiver.first, receiver.second, SurfaceProperties{})
                .radiance;
        const Vec3 b =
            distant.system.indirect_diffuse(receiver.first, receiver.second, SurfaceProperties{})
                .radiance;
        CY_CHECK_GT(a.x + a.y + a.z, 0.0F);
        CY_CHECK_EQ(a.x, b.x);
        CY_CHECK_EQ(a.y, b.y);
        CY_CHECK_EQ(a.z, b.z);
    }
}

CY_TEST_CASE("a GI volume scales indirect intensity where it reaches") {
    Room none;
    Room scaled;
    GiVolume volume;
    volume.bounds = Aabb::from_center_extents(kQuery, Vec3{1.0F, 1.0F, 1.0F});
    volume.blend_distance_metres = 0.0F;
    volume.sets = gi_volume_bit(GiVolumeParameter::DiffuseIntensity) |
                  gi_volume_bit(GiVolumeParameter::SpecularIntensity);
    volume.diffuse_intensity = 0.5F;
    volume.specular_intensity = 2.0F;
    CY_REQUIRE(scaled.system.set_volumes({&volume, 1}).has_value());
    none.converge();
    scaled.converge();

    // Intensity: exactly the factor, over the same probes (an intensity places nothing).
    SurfaceProperties surface;
    const Vec3 base = none.system.indirect_diffuse(kQuery, kUp, surface).radiance;
    const Vec3 half = scaled.system.indirect_diffuse(kQuery, kUp, surface).radiance;
    CY_REQUIRE(base.y > 0.0F);
    CY_CHECK_EQ(half.x, base.x * 0.5F);
    CY_CHECK_EQ(half.y, base.y * 0.5F);
    CY_CHECK_EQ(half.z, base.z * 0.5F);

    // Specular: twice the answer, through the same tracer and budget.
    cy::rendering::gi::TraceBudget budget_a;
    cy::rendering::gi::TraceBudget budget_b;
    const Vec3 view = cy::normalize(Vec3{1.0F, -1.0F, 0.3F});
    const Vec3 spec = none.system.indirect_specular(kQuery, kUp, view, 0.2F, 3, budget_a).radiance;
    const Vec3 doubled =
        scaled.system.indirect_specular(kQuery, kUp, view, 0.2F, 3, budget_b).radiance;
    CY_REQUIRE(spec.y > 0.0F);
    CY_CHECK_EQ(doubled.y, spec.y * 2.0F);
}

CY_TEST_CASE("a GI volume moves the far-field transition where it reaches") {
    // Outdoors, where the sky is seen: at 20 m from the camera the volume's 12 m transition
    // resolves exactly as a system configured with 12 m globally, and not as the 60 m default.
    Interiors global(60.0F, 2);
    Interiors configured(12.0F, 2);
    Interiors overridden(60.0F, 2);
    GiVolume volume;
    volume.bounds = Aabb::from_center_extents(kCamera, Vec3{1.0F, 1.0F, 1.0F});
    volume.blend_distance_metres = 0.0F;
    volume.sets = gi_volume_bit(GiVolumeParameter::FarFieldDistance);
    volume.far_field_begins_metres = 12.0F;
    CY_REQUIRE(overridden.system.set_volumes({&volume, 1}).has_value());
    for (Interiors* scene : {&global, &configured, &overridden}) {
        scene->add_floor();
    }
    SurfaceProperties surface;
    surface.camera_distance_metres = 20.0F;
    const Vec3 a = global.system.indirect_diffuse(kCamera, kUp, surface).radiance;
    const Vec3 b = configured.system.indirect_diffuse(kCamera, kUp, surface).radiance;
    const Vec3 c = overridden.system.indirect_diffuse(kCamera, kUp, surface).radiance;
    CY_CHECK_NE(b.y, a.y);
    CY_CHECK_EQ(c.x, b.x);
    CY_CHECK_EQ(c.y, b.y);
    CY_CHECK_EQ(c.z, b.z);
    CY_CHECK_EQ(overridden.system.region_settings(kCamera).far_field_begins_metres, 12.0F);
    CY_CHECK_EQ(overridden.system.region_settings(kLeft).far_field_begins_metres, 60.0F);
}

CY_TEST_CASE("a GI volume that prefers the software tier keeps its rays off the hardware tier") {
    Room room;
    room.converge();
    GiVolume volume;
    volume.bounds = Aabb::from_center_extents(kQuery, Vec3{1.0F, 1.0F, 1.0F});
    volume.blend_distance_metres = 0.0F;
    volume.sets = gi_volume_bit(GiVolumeParameter::TierPreference);
    volume.tier = GiTierPreference::SoftwareOnly;
    CY_REQUIRE(room.system.set_volumes({&volume, 1}).has_value());
    CY_REQUIRE_EQ(room.system.tracer().selected_world_tier(),
                  cy::rendering::gi::RadianceSource::HardwareTrace);

    const Vec3 view = cy::normalize(Vec3{1.0F, -1.0F, 0.3F});
    const auto rays = [&](Vec3 at) {
        room.system.tracer().reset_diagnostics();
        cy::rendering::gi::TraceBudget budget;
        (void)room.system.indirect_specular(at, kUp, view, 0.2F, 3, budget);
        return room.system.tracer().diagnostics();
    };
    const auto inside = rays(kQuery);
    CY_CHECK_GT(inside.software_rays, 0U);
    CY_CHECK_EQ(inside.hardware_rays, 0U);
    const auto outside = rays(Vec3{2.0F, -1.5F, 2.0F});
    CY_CHECK_GT(outside.hardware_rays, 0U);
    CY_CHECK_EQ(outside.software_rays, 0U);

    // And its probes, placed again by the change, gather through the software tier too: a frame
    // that updates every probe traces exactly their rays fewer through the hardware tier than the
    // same frame in a room with no volume.
    Room control;
    control.converge();
    const auto gi_rays = [](Room& scene) {
        return scene.system.acceleration()
            .diagnostics()
            .rays[static_cast<u32>(cy::rendering::rt::Consumer::GlobalIllumination)];
    };
    FrameContext all;
    all.converged_mode = true;
    all.frame = 50;
    const u64 control_before = gi_rays(control);
    const auto control_report = control.system.update(all);
    const u64 control_hardware = gi_rays(control) - control_before;
    const u64 before = gi_rays(room);
    const auto report = room.system.update(all);
    const u64 hardware = gi_rays(room) - before;
    u32 software_probes = 0;
    for (const Probe& probe : room.system.radiance().probes()) {
        if (probe.live && volume.bounds.contains(probe.position)) {
            CY_CHECK_EQ(probe.tier, GiTierPreference::SoftwareOnly);
            software_probes += 1;
        }
    }
    CY_REQUIRE(software_probes > 0U);
    CY_REQUIRE_EQ(report.probes.probes_updated, control_report.probes.probes_updated);
    const u64 rays_per_probe = report.probes.rays / report.probes.probes_updated;
    CY_CHECK_EQ(control_hardware - hardware, software_probes * rays_per_probe);
}
