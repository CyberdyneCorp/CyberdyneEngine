// M7's exit criterion: ray tracing disabled falls back to software tracing with no visual
// discontinuity beyond tolerance. Task 9.5.
//
// ================================================================================================
// WHAT IS AND IS NOT MEASURED HERE, SAID BEFORE THE NUMBERS
// ================================================================================================
//
// The hardware tier is `ray-tracing-infrastructure`'s ray query. On this tree that query executes
// over `cy::Bvh` on the CPU, because `cy::rhi::Capability::RayTracing` is an enumerator nothing
// sets and layer 4 may not name a Vulkan header — src/rendering/raytracing/README.md carries both
// facts and how to check them. So what this file measures is the difference between two
// INTERSECTION METHODS over one scene: exact triangles through the ray query, against sphere
// tracing a sparse signed distance field. That is the whole of what the fallback changes, and it is
// the difference a player would see.
//
// What it does NOT measure is a device. When the RHI grows an acceleration-structure API, the same
// comparison runs unchanged with a device behind the query, and the number it prints is the number
// that will move.
//
// ================================================================================================
// THE STATISTIC THIS FILE LEADS WITH
// ================================================================================================
//
// The relative error of the mean, over every sample point. Not the maximum: a maximum over a
// stochastic gather is a single draw, it does not reproduce, and M7's own task 5b.5b exists because
// a previous milestone's artefact led with one. The maximum is printed BESIDE it, because a
// discontinuity has to show up somewhere — but the assertion and the headline are the mean.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/system.h>

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

/// The tolerance the criterion is expressed in: the mean relative difference between the software
/// and hardware tiers' resolved indirect diffuse, over the sample grid.
///
/// Ten per cent. The two tiers intersect the same room by two different methods — exact triangles
/// against a sparse field whose finest voxel is half a metre here — so they are not expected to
/// agree exactly, and a tolerance tight enough to fail on the discretisation would be a tolerance
/// that fails on the voxel size rather than on a defect. The measured value on this machine is
/// printed below the assertion so a regression shows as a number rather than as a pass or a fail.
constexpr f32 kTierTolerance = 0.10F;

/// How much more a profile switch may move the image than an ordinary frame does.
///
/// The switch is measured against a CONTROL — the same scene, one more frame, no switch — because
/// a hybrid renderer's image keeps moving after the world stops, and a step measured against
/// nothing would be measuring the convergence rather than the switch. 1.5 says the switch may cost
/// half again what standing still costs, and the two numbers are printed so a regression is visible
/// as a ratio rather than as a threshold.
constexpr f32 kSwitchOverControl = 1.5F;

/// A closed room with no divider: the two representations differ only by discretisation, which is
/// what makes the measured difference a property of the tiers rather than of the fixture.
struct FallbackRoom {
    gi_support::RoomField field{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    gi_support::RoomTriangles triangles{false};
    std::vector<Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<GiLight> lights = gi_support::room_lights();
    IlluminationSystem system;

    explicit FallbackRoom(bool device_ray_tracing) {
        IlluminationSettings settings = gi_support::room_settings();
        settings.ray_tracing.device_supports_ray_tracing = device_ray_tracing;
        settings.ray_tracing.budget.max_builds_per_frame = 8;
        CY_REQUIRE(system.configure(settings).has_value());

        CY_REQUIRE(system.field().place(1, field.asset(), cy::Mat4::identity()).has_value());

        const cy::Aabb bounds = cy::Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
        CY_REQUIRE(
            system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
        system.surfaces().set_lookup_radius(1.2F);

        // The same room, as triangles, for the hardware tier. Declared whatever the capability
        // says: a consumer's residency path does not branch on it, and an inactive service builds
        // nothing from the declaration.
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

/// The points the two tiers are compared at: a grid over the floor and up the walls, away from the
/// corners where either representation is least like the other.
[[nodiscard]] std::vector<std::pair<Vec3, Vec3>> sample_points() {
    std::vector<std::pair<Vec3, Vec3>> points;
    const auto span = [](u32 index) { return -2.5F + (static_cast<f32>(index) * 1.25F); };
    for (u32 ix = 0; ix < 5; ++ix) {
        const f32 x = span(ix);
        for (u32 iz = 0; iz < 5; ++iz) {
            const f32 z = span(iz);
            points.emplace_back(Vec3{x, -gi_support::kRoomY + 0.1F, z}, Vec3{0.0F, 1.0F, 0.0F});
            points.emplace_back(Vec3{x, gi_support::kRoomY - 0.1F, z}, Vec3{0.0F, -1.0F, 0.0F});
        }
    }
    for (u32 iy = 0; iy < 3; ++iy) {
        const f32 y = -1.0F + static_cast<f32>(iy);
        for (u32 iz = 0; iz < 5; ++iz) {
            const f32 z = span(iz);
            points.emplace_back(Vec3{-gi_support::kRoomX + 0.1F, y, z}, Vec3{1.0F, 0.0F, 0.0F});
            points.emplace_back(Vec3{gi_support::kRoomX - 0.1F, y, z}, Vec3{-1.0F, 0.0F, 0.0F});
        }
    }
    return points;
}

[[nodiscard]] std::vector<Vec3> resolve_all(const IlluminationSystem& system,
                                            const std::vector<std::pair<Vec3, Vec3>>& points) {
    std::vector<Vec3> values;
    values.reserve(points.size());
    const SurfaceProperties surface;
    for (const auto& point : points) {
        values.push_back(system.indirect_diffuse(point.first, point.second, surface).radiance);
    }
    return values;
}

}  // namespace

CY_TEST_CASE("with ray tracing disabled the software tier takes over, and it is the default here") {
    // The premise, checked rather than assumed: on this tree no device reports ray tracing, so the
    // fallback is not an exotic path — it is the only one that runs.
    FallbackRoom absent(false);
    CY_CHECK_EQ(absent.system.acceleration().availability(),
                cy::rendering::rt::Availability::Unsupported);
    CY_CHECK_EQ(absent.system.world_tracer().tier(), RadianceSource::SoftwareTrace);

    FallbackRoom present(true);
    CY_CHECK_EQ(present.system.acceleration().availability(),
                cy::rendering::rt::Availability::Available);
    CY_CHECK_EQ(present.system.world_tracer().tier(), RadianceSource::HardwareTrace);
}

CY_TEST_CASE("the two tiers agree within tolerance over the whole room") {
    FallbackRoom software(false);
    FallbackRoom hardware(true);
    const auto points = sample_points();

    // Identical inputs, identical frame counts, and converged mode so neither answer is a
    // half-settled one. Anything else would measure the schedulers rather than the tiers.
    constexpr u32 kFrames = 12;
    (void)software.system.advance_to_convergence(software.context(0), 0.95F, kFrames);
    (void)hardware.system.advance_to_convergence(hardware.context(0), 0.95F, kFrames);

    // The hardware tier really did trace: without this the comparison could pass because both sides
    // ran the same software tier.
    CY_CHECK_GT(hardware.system.acceleration()
                    .diagnostics()
                    .rays[static_cast<u32>(cy::rendering::rt::Consumer::GlobalIllumination)],
                0U);
    CY_CHECK_EQ(software.system.acceleration()
                    .diagnostics()
                    .rays[static_cast<u32>(cy::rendering::rt::Consumer::GlobalIllumination)],
                0U);

    const std::vector<Vec3> software_values = resolve_all(software.system, points);
    const std::vector<Vec3> hardware_values = resolve_all(hardware.system, points);
    const ReferenceComparison comparison =
        compare_against_reference({software_values.data(), software_values.size()},
                                  {hardware_values.data(), hardware_values.size()});

    CY_REQUIRE_EQ(comparison.samples, static_cast<u32>(points.size()));
    CY_CHECK_GT(comparison.mean_reference_magnitude, 0.0F);

    // The headline: the mean relative difference. Printed so a regression is a number rather than a
    // pass or a fail, and the maximum beside it because a discontinuity has to show up somewhere.
    std::printf(
        "\n  fallback: software vs hardware tier over %u points\n"
        "    mean relative difference   %.4f   (tolerance %.2f)\n"
        "    mean absolute difference   %.5f\n"
        "    max  absolute difference   %.5f   (a single draw; not the criterion)\n"
        "    mean reference magnitude   %.5f\n\n",
        comparison.samples, static_cast<double>(comparison.relative_error),
        static_cast<double>(kTierTolerance), static_cast<double>(comparison.mean_absolute_error),
        static_cast<double>(comparison.max_absolute_error),
        static_cast<double>(comparison.mean_reference_magnitude));

    CY_CHECK_LT(comparison.relative_error, kTierTolerance);
}

CY_TEST_CASE("turning ray tracing off mid-run is not a visual discontinuity") {
    // The criterion as a player would meet it: a profile switch on capable hardware.
    //
    // Two identical rooms, converged identically. One takes an ordinary next frame; the other takes
    // its next frame on the software tier. The difference between the two steps is what the switch
    // cost, and the control is what makes that a measurement — a hybrid renderer's image keeps
    // moving after the world stops, and a step measured against nothing measures the convergence.
    FallbackRoom control(true);
    FallbackRoom switched(true);
    const auto points = sample_points();

    (void)control.system.advance_to_convergence(control.context(0), 0.95F, 12);
    (void)switched.system.advance_to_convergence(switched.context(0), 0.95F, 12);
    CY_REQUIRE_EQ(switched.system.world_tracer().tier(), RadianceSource::HardwareTrace);

    const std::vector<Vec3> before = resolve_all(switched.system, points);
    const std::vector<Vec3> control_before = resolve_all(control.system, points);
    // The two rooms are the same room: deterministic, and identical up to nothing at all.
    CY_REQUIRE_EQ(before.size(), control_before.size());
    for (cy::usize index = 0; index < before.size(); ++index) {
        CY_CHECK_EQ(before[index].x, control_before[index].x);
    }

    (void)control.system.update(control.context(13));
    const std::vector<Vec3> control_after = resolve_all(control.system, points);

    switched.system.acceleration().set_enabled_by_profile(false);
    CY_CHECK_EQ(switched.system.acceleration().availability(),
                cy::rendering::rt::Availability::DisabledByProfile);
    CY_CHECK_EQ(switched.system.world_tracer().tier(), RadianceSource::SoftwareTrace);
    (void)switched.system.update(switched.context(13));
    const std::vector<Vec3> after = resolve_all(switched.system, points);

    const ReferenceComparison ordinary =
        compare_against_reference({control_after.data(), control_after.size()},
                                  {control_before.data(), control_before.size()});
    const ReferenceComparison step =
        compare_against_reference({after.data(), after.size()}, {before.data(), before.size()});
    std::printf(
        "  profile switch: hardware -> software, one frame later\n"
        "    an ordinary frame moves     %.4f\n"
        "    the switching frame moves   %.4f   (at most %.1fx the ordinary one)\n"
        "    max absolute step           %.5f\n\n",
        static_cast<double>(ordinary.relative_error), static_cast<double>(step.relative_error),
        static_cast<double>(kSwitchOverControl), static_cast<double>(step.max_absolute_error));
    CY_CHECK_LT(step.relative_error, ordinary.relative_error * kSwitchOverControl);

    // And back again, which must not rebuild anything: a profile change is not a residency event.
    // While disabled the service REPORTS what an unsupported one reports — no structures and no
    // memory — because anything else would be an observable difference between the two causes.
    const u64 structures = control.system.acceleration().diagnostics().structure_bytes;
    CY_CHECK_GT(structures, 0U);
    CY_CHECK_EQ(switched.system.acceleration().diagnostics().structure_bytes, 0U);
    switched.system.acceleration().set_enabled_by_profile(true);
    CY_CHECK_EQ(switched.system.world_tracer().tier(), RadianceSource::HardwareTrace);
    const auto report = switched.system.update(switched.context(14));
    CY_CHECK_EQ(report.acceleration.builds, 0U);
    CY_CHECK_EQ(report.acceleration.rebuilds, 0U);
    CY_CHECK_EQ(switched.system.acceleration().diagnostics().structure_bytes, structures);
}

CY_TEST_CASE("a device without ray tracing and a profile that disabled it are indistinguishable") {
    // The other half of the same requirement, at the illumination level rather than the service
    // level: the two causes must produce one behaviour, and here that means one image.
    FallbackRoom unsupported(false);
    FallbackRoom disabled(true);
    disabled.system.acceleration().set_enabled_by_profile(false);

    const auto points = sample_points();
    (void)unsupported.system.advance_to_convergence(unsupported.context(0), 0.95F, 12);
    (void)disabled.system.advance_to_convergence(disabled.context(0), 0.95F, 12);

    const std::vector<Vec3> a = resolve_all(unsupported.system, points);
    const std::vector<Vec3> b = resolve_all(disabled.system, points);
    CY_REQUIRE_EQ(a.size(), b.size());
    // Bit for bit. Not "within tolerance": the two ran the same code over the same data, and a
    // tolerance here would hide the day they stop.
    for (cy::usize index = 0; index < a.size(); ++index) {
        CY_CHECK_EQ(a[index].x, b[index].x);
        CY_CHECK_EQ(a[index].y, b[index].y);
        CY_CHECK_EQ(a[index].z, b[index].z);
    }
}

CY_TEST_CASE(
    "the software tier is measured against the path-traced reference, not against itself") {
    // "The real-time result SHALL be measurable against the reference." The reference is the same
    // path tracer the bake uses, over the same GI scene, through the same tier — so what this
    // number measures is the real-time approximation and not a second representation.
    FallbackRoom room(false);
    (void)room.system.advance_to_convergence(room.context(0), 0.95F, 12);

    const WorldTracer& tracer = room.system.world_tracer();
    const PathTracer path(tracer, room.system.scene(), {room.lights.data(), room.lights.size()},
                          SkyTerm{}, &tracer);
    BakeSettings settings;
    settings.bounces = 2;
    settings.samples = 24;
    settings.max_distance_metres = 24.0F;

    const auto points = sample_points();
    std::vector<Vec3> reference;
    reference.reserve(points.size());
    for (const auto& point : points) {
        reference.push_back(path.irradiance(point.first, point.second, settings));
    }
    const std::vector<Vec3> measured = resolve_all(room.system, points);

    const ReferenceComparison comparison = compare_against_reference(
        {measured.data(), measured.size()}, {reference.data(), reference.size()});
    std::printf(
        "  real-time vs path-traced reference over %u points\n"
        "    mean relative error        %.4f\n"
        "    mean absolute error        %.5f\n\n",
        comparison.samples, static_cast<double>(comparison.relative_error),
        static_cast<double>(comparison.mean_absolute_error));

    // The assertion is deliberately loose and the number is the point: a real-time hybrid gather at
    // this probe density is not a path tracer, and pretending otherwise with a tight bound would
    // make this a test of the fixture. What must hold is that the reference is non-trivial and the
    // real-time answer is the same order of magnitude — a bug that darkened or blew out the gather
    // moves this immediately.
    CY_CHECK_GT(comparison.mean_reference_magnitude, 0.0F);
    CY_CHECK_LT(comparison.relative_error, 1.5F);
}
