// One frame of the composed system: the modes, the invalidations, the bake's seeds, and
// convergence. Tasks 9.1 and 9.2.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/system.h>

#include "support.h"

#include <cmath>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

/// A room, its field, its cards and its light, wired into an illumination system.
struct Room {
    gi_support::RoomField field{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    std::vector<Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<GiLight> lights = gi_support::room_lights();
    IlluminationSystem system;

    explicit Room(bool device_ray_tracing = false) {
        IlluminationSettings settings = gi_support::room_settings();
        settings.ray_tracing.device_supports_ray_tracing = device_ray_tracing;
        CY_REQUIRE(system.configure(settings).has_value());
        CY_REQUIRE(system.field().place(1, field.asset(), cy::Mat4::identity()).has_value());
        const cy::Aabb bounds = cy::Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
        CY_REQUIRE(
            system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
        system.surfaces().set_lookup_radius(1.2F);
    }

    [[nodiscard]] FrameContext context(u64 frame) const noexcept {
        FrameContext ctx;
        ctx.camera = Vec3{0.0F, 0.0F, 0.0F};
        ctx.lights = {lights.data(), lights.size()};
        ctx.frame = frame;
        ctx.measured_gi_ms = 1.0F;
        return ctx;
    }

    void run(u32 frames) {
        for (u32 index = 0; index < frames; ++index) {
            (void)system.update(context(index));
        }
    }
};

}  // namespace

CY_TEST_CASE("a frame runs every subsystem and the software tier answers on this machine") {
    Room room;
    const IlluminationFrameReport report = room.system.update(room.context(0));

    CY_CHECK_GT(report.field.bricks_solved, 0U);
    CY_CHECK_GT(report.placement.probes_created, 0U);
    CY_CHECK_GT(report.surfaces.pages_updated, 0U);
    CY_CHECK_GT(report.probes.probes_updated, 0U);
    // The acceleration service is inactive here — no device reports ray tracing — so the fallback
    // is the path that runs, and this is the assertion that says so rather than assuming it.
    CY_CHECK_EQ(room.system.acceleration().availability(),
                cy::rendering::rt::Availability::Unsupported);
    CY_CHECK_EQ(report.world_tier, RadianceSource::SoftwareTrace);
    CY_CHECK_EQ(report.acceleration.builds, 0U);
    // And the cell ingestion's own invalidation was serviced by every subsystem that holds state.
    CY_CHECK_GT(report.invalidations_serviced, 0U);
}

CY_TEST_CASE("indirect diffuse is answered, and each wall tints the floor beside it") {
    Room room;
    // The light goes to the middle of the room for this case. With it in one corner the red wall is
    // lit and the blue one is not, and the floor by the blue wall would be redder — which is
    // correct physics and a useless test of colour bleeding.
    room.lights[0].position = Vec3{0.0F, 1.0F, 0.0F};
    room.run(24);

    const SurfaceProperties surface;
    const ResolveResult beside_red =
        room.system.indirect_diffuse(Vec3{-3.0F, -1.6F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, surface);
    const ResolveResult beside_blue =
        room.system.indirect_diffuse(Vec3{3.0F, -1.6F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, surface);

    CY_CHECK_GT(beside_red.confidence, 0.0F);
    CY_CHECK_NE(beside_red.sources_used & source_bit(RadianceSource::RadianceCache), 0U);
    // The floor by the red wall is redder than the floor by the blue one, and vice versa. That is
    // colour bleeding, and it comes from the surface cache's recorded reflectance.
    const f32 red_bias = beside_red.radiance.x - beside_red.radiance.z;
    const f32 blue_bias = beside_blue.radiance.z - beside_blue.radiance.x;
    CY_CHECK_GT(red_bias, 0.0F);
    CY_CHECK_GT(blue_bias, 0.0F);
}

CY_TEST_CASE("the GI mode decides what runs, and None is a mode rather than a failure") {
    Room room;
    room.system.set_mode(GiMode::None);
    const IlluminationFrameReport none = room.system.update(room.context(0));
    CY_CHECK_EQ(none.probes.probes_updated, 0U);
    CY_CHECK_EQ(none.surfaces.pages_updated, 0U);
    // The field and the structures still run: they are the world, not the illumination.
    CY_CHECK_GT(none.field.bricks_solved, 0U);

    // And the resolve in `None` takes the sky and excludes every bounce.
    const SurfaceProperties surface;
    const ResolveResult ambient =
        room.system.indirect_diffuse(Vec3{0.0F, -1.6F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, surface);
    CY_CHECK_EQ(ambient.sources_used & source_bit(RadianceSource::RadianceCache), 0U);

    // `Baked` places probes and updates neither cache: the seeds are the answer.
    room.system.set_mode(GiMode::Baked);
    const IlluminationFrameReport baked = room.system.update(room.context(1));
    CY_CHECK_GT(baked.placement.probes_created + baked.placement.probes_reused, 0U);
    CY_CHECK_EQ(baked.probes.probes_updated, 0U);
    CY_CHECK_EQ(baked.surfaces.pages_updated, 0U);
}

CY_TEST_CASE("a light that changes invalidates its region and nothing else") {
    Room room;
    room.run(8);
    const u32 pages = room.system.surfaces().diagnostics().page_count;

    // The light switches colour: a local change with a stated cause and a stated source.
    room.system.scene().invalidate(
        cy::Aabb::from_center_extents(Vec3{-2.0F, 1.0F, -2.0F}, Vec3{1.5F, 1.5F, 1.5F}),
        InvalidationCause::LightChanged, room.lights[0].id, 8);
    const IlluminationFrameReport report = room.system.update(room.context(8));

    CY_CHECK_EQ(report.invalidations_serviced, 1U);
    CY_CHECK_GT(report.invalidated_surface_pages, 0U);
    // Local, not global: a fraction of the cache, not all of it.
    CY_CHECK_LT(report.invalidated_surface_pages, pages / 2U);
    CY_CHECK_GT(report.invalidated_field_bricks, 0U);
}

CY_TEST_CASE("a bake seeds the caches so the first frame is not black") {
    Room seeded;
    Room unseeded;

    const WorldTracer& tracer = seeded.system.world_tracer();
    // The field has to be solved before the path tracer can trace it.
    (void)seeded.system.update(seeded.context(0));
    const PathTracer path(tracer, seeded.system.scene(),
                          {seeded.lights.data(), seeded.lights.size()}, SkyTerm{}, &tracer);

    BakeSettings settings;
    settings.bounces = 1;
    settings.samples = 8;
    settings.max_distance_metres = 24.0F;
    const auto report =
        seed_caches(path, seeded.system.surfaces(), seeded.system.radiance(),
                    &seeded.system.reflection_probes(),
                    {seeded.lights.data(), seeded.lights.size()}, &tracer, settings);
    CY_REQUIRE(report.has_value());
    CY_CHECK_GT(report.value().probes_seeded, 0U);
    CY_CHECK_GT(report.value().surface_pages_seeded, 0U);
    CY_CHECK_GT(report.value().rays, 0U);

    // Every probe is valid immediately. The comparison is against a system that has placed its
    // probes and not yet gathered: without a bake a probe starts invalid and converges, and that is
    // the difference the seeding removes.
    CY_CHECK_EQ(seeded.system.radiance().diagnostics().valid_probes,
                seeded.system.radiance().probe_count());
    ProbePlacementContext placement;
    placement.field = &unseeded.system.field();
    (void)unseeded.system.field().scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    (void)unseeded.system.radiance().scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);
    CY_CHECK_GT(unseeded.system.radiance().probe_count(), 0U);
    CY_CHECK_EQ(unseeded.system.radiance().diagnostics().valid_probes, 0U);

    // And the seeded system answers a query on its first frame.
    const SurfaceProperties surface;
    const ResolveResult first =
        seeded.system.indirect_diffuse(Vec3{-2.0F, -1.6F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, surface);
    CY_CHECK_GT(first.confidence, 0.0F);
    CY_CHECK_GT(cy::length(first.radiance), 0.0F);
}

CY_TEST_CASE("converged mode reaches a stable state and reports how far it is from one") {
    // "Converged mode SHALL be used by golden-image tests, cinematic capture, and reference
    // comparison, so temporal convergence does not make rendering tests unreliable."
    Room room;
    const u32 frames = room.system.advance_to_convergence(room.context(0), 0.9F, 24);
    CY_CHECK_LE(frames, 24U);
    CY_CHECK_GT(room.system.convergence().region_count(), 0U);
    CY_CHECK_GE(room.system.convergence().worst_convergence(), 0.0F);

    // Repeated CAPTURES produce the same image, which is the property a golden image depends on —
    // two runs of the same script from the same start, not one run continued. Continuing is not the
    // same question: a converged system is one that has stopped moving much, not one that has
    // stopped moving, and asking it to be idempotent would be asking convergence to be exact.
    Room second;
    const u32 again = second.system.advance_to_convergence(second.context(0), 0.9F, 24);
    CY_CHECK_EQ(again, frames);

    const SurfaceProperties surface;
    const ResolveResult once =
        room.system.indirect_diffuse(Vec3{0.0F, -1.6F, 1.0F}, Vec3{0.0F, 1.0F, 0.0F}, surface);
    const ResolveResult twice =
        second.system.indirect_diffuse(Vec3{0.0F, -1.6F, 1.0F}, Vec3{0.0F, 1.0F, 0.0F}, surface);
    // Bit for bit: the whole path is deterministic, and a golden image that only matched to a
    // tolerance would be hiding whichever part of it was not.
    CY_CHECK_EQ(once.radiance.x, twice.radiance.x);
    CY_CHECK_EQ(once.radiance.y, twice.radiance.y);
    CY_CHECK_EQ(once.radiance.z, twice.radiance.z);
}

CY_TEST_CASE("reflections take the same tiers and the roughness rule decides the cost") {
    Room room;
    room.run(8);

    // A mirror issues dedicated rays through the same tiered tracer; a rough surface issues none
    // and reads the cache. The ray counters are how that is visible from outside.
    room.system.tracer().reset_diagnostics();
    TraceBudget budget;
    const ResolveResult mirror = room.system.indirect_specular(
        Vec3{0.0F, -1.5F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}, 0.02F, 1, budget);
    const u32 mirror_rays = room.system.tracer().diagnostics().software_rays;

    room.system.tracer().reset_diagnostics();
    TraceBudget rough_budget;
    const ResolveResult rough =
        room.system.indirect_specular(Vec3{0.0F, -1.5F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                                      Vec3{0.0F, -1.0F, 0.0F}, 0.95F, 1, rough_budget);
    const u32 rough_rays = room.system.tracer().diagnostics().software_rays;

    CY_CHECK_GT(mirror_rays, 0U);
    CY_CHECK_EQ(rough_rays, 0U);
    CY_CHECK_GT(mirror.confidence, 0.0F);
    // The rough answer still exists: it comes from the radiance cache, which costs nothing extra
    // because the diffuse gather already pays for it.
    CY_CHECK_GT(rough.confidence, 0.0F);
    CY_CHECK_NE(rough.sources_used & source_bit(RadianceSource::RadianceCache), 0U);
}

CY_TEST_CASE("the budget's decisions reach the subsystems that spend the frame") {
    Room room;
    room.run(4);
    const u32 full_probe_budget =
        room.system.budget().value(GiLever::ProbeUpdates) > 0.0F
            ? static_cast<u32>(room.system.budget().value(GiLever::ProbeUpdates))
            : 0U;
    CY_CHECK_GT(full_probe_budget, 0U);

    // Drive the system far over its allocation. The levers walk their declared order and the probe
    // update rate comes down with them.
    FrameContext over = room.context(5);
    over.measured_gi_ms = 40.0F;
    for (u64 frame = 5; frame < 40; ++frame) {
        over.frame = frame;
        (void)room.system.update(over);
    }
    CY_CHECK_GT(room.system.budget().position(GiLever::DenoiserQuality), 0U);
    CY_CHECK_LT(static_cast<u32>(room.system.budget().value(GiLever::ProbeUpdates)),
                full_probe_budget);
    CY_CHECK(room.system.budget().at_reserved_minimum());
    // Coarser, never absent: the last rung still updates probes and still denoises.
    CY_CHECK_GT(static_cast<u32>(room.system.budget().value(GiLever::ProbeUpdates)), 0U);
    CY_CHECK_GT(static_cast<u32>(room.system.budget().value(GiLever::TracedRays)), 0U);
}
