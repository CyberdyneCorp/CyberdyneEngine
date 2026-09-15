// THE SEAM [dependency cycle 2](docs/roadmap/dependencies.md) IS ABOUT, RUN. M11.c tasks 2.1–2.4.
//
// ================================================================================================
// WHAT WAS TRUE OF THIS TREE BEFORE THIS FILE, AND WHY THAT MAKES EVERY CASE HERE A CONTROL
// ================================================================================================
//
// `rendering-global-illumination` has thirteen scenarios under "Sky and atmosphere" and every one of
// them was satisfied, on a tree where `gi::SkyTerm` was a two-colour gradient nobody built from an
// atmosphere, by that gradient. `atmosphere-sky-and-clouds` measured `fit_sky_gradient` against the
// atmosphere's own irradiance integral — 12.4 % mean — in its own suite, with no illumination system
// present. Both halves were real and tested; the JOIN did not exist, and no check in fifteen
// ledgers could say so.
//
// So the cases below are written against the two ways a join can be fake rather than absent:
//
//   1. A TERM THAT IS CONSTRUCTED AND NOT READ. The composition point builds a `SkyTerm` from the
//      atmosphere, installs it, and the illumination system goes on answering out of whatever it
//      was configured with. Every case that asserts a provenance therefore also asserts that the
//      PICTURE MOVED — `indirect_diffuse` at a ground point, which is what a surface actually sees.
//   2. A TERM THAT IS THE ATMOSPHERE'S NAME ON A CONSTANT. Returning a plausible gradient would
//      satisfy "built from the atmosphere" against any assertion about its shape, so the fitted
//      term is compared against `sky::sky_irradiance` — the atmosphere's own hemispherical integral,
//      computed independently of the fit — at two sun elevations that differ by a factor.
//
// A third fake is not available to this file and is checked one level up, in the criterion: a
// composition point that exists as a test fixture. `m11c:gi-sky-term-constructed` requires the
// module to be a LIBRARY in the link graph, and `src/rendering/sky_illumination/CMakeLists.txt` is
// the only one in this tree that depends on `cy::rendering-gi` and `cy::rendering-sky` at once.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky_illumination/sky_illumination.h>

#include "support.h"

#include <cmath>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::rendering::gi::IlluminationFrameReport;
using cy::rendering::gi::SkyTerm;
using cy::rendering::gi::SurfaceProperties;
using cy::rendering::skylight::SkyIllumination;
using cy::rendering::skylight::SkyIlluminationFrame;
using cy::rendering::skylight::SkyIlluminationReport;
using cy::rendering::skylight::SkyIlluminationSettings;
using cy::rendering::skylight::SkyTermProvenance;
using cy::rendering::skylight::term_irradiance;
using sky_illumination_support::Ground;

/// The point a surface sees the sky from: the middle of the slab's top, facing up.
constexpr Vec3 kQueryPoint{0.0F, 0.02F, 0.0F};
constexpr Vec3 kUp{0.0F, 1.0F, 0.0F};

[[nodiscard]] f32 magnitude(Vec3 value) noexcept {
    return length(value);
}

/// What the ground actually receives, through the illumination system's own resolve. The number
/// that moves when the sky term moves, and does not when the term is installed somewhere nothing
/// reads.
[[nodiscard]] f32 ground_indirect(const cy::rendering::gi::IlluminationSystem& system) noexcept {
    SurfaceProperties surface;
    surface.camera_distance_metres = 2.0F;
    return magnitude(system.indirect_diffuse(kQueryPoint, kUp, surface).radiance);
}

[[nodiscard]] SkyIlluminationFrame frame_at(Vec3 sun, u64 frame) noexcept {
    SkyIlluminationFrame out;
    out.sun_direction = sun;
    out.lit_region = Ground::bounds();
    out.frame = frame;
    return out;
}

/// A sun that climbs from the horizon. `elevation` in radians.
[[nodiscard]] Vec3 sun_at(f32 elevation) noexcept {
    return normalize(Vec3{std::cos(elevation) * 0.6F, std::sin(elevation), std::cos(elevation) * 0.8F});
}

}  // namespace

CY_TEST_CASE("the sky term is built from the atmosphere, and a fallback says it is one") {
    Ground ground;
    ground.run(4);

    // --- The declared fallback, first, so the atmosphere has something to be different from -----
    SkyIllumination seam;
    CY_REQUIRE(seam.configure(SkyIlluminationSettings{}).has_value());
    const SkyTerm placeholder{Vec3{0.05F, 0.06F, 0.09F}, Vec3{0.08F, 0.08F, 0.09F},
                              Vec3{0.02F, 0.02F, 0.02F}, 1.0F};
    seam.use_analytic_fallback(placeholder);

    const Vec3 sun = sun_at(0.6F);
    SkyIlluminationReport report = seam.update(frame_at(sun, 0), ground.system);
    CY_CHECK_EQ(report.provenance, SkyTermProvenance::AnalyticFallback);
    CY_CHECK_FALSE(report.sun_from_atmosphere);
    CY_CHECK_FALSE(report.refitted);
    (void)ground.system.update(ground.context(4));
    const f32 with_placeholder = ground_indirect(ground.system);

    // --- The atmosphere ---------------------------------------------------------------------------
    CY_REQUIRE(seam.set_atmosphere(cy::rendering::sky::earth_atmosphere()).has_value());
    report = seam.update(frame_at(sun, 5), ground.system);
    CY_CHECK_EQ(report.provenance, SkyTermProvenance::PhysicalAtmosphere);
    CY_CHECK(report.refitted);
    CY_CHECK(report.invalidation_filed);
    CY_CHECK(report.sun_from_atmosphere);
    CY_CHECK_GT(report.sun.intensity, 0.0F);
    CY_CHECK(report.sun.directional);
    CY_CHECK_GT(report.fits, 0U);
    CY_CHECK_GT(report.directions_integrated, 0ULL);

    // THE TERM REACHED THE SYSTEM. Not "a term was built": the illumination system's own copy is
    // the one that was fitted, which is what `set_sky_term` exists to make true without paying for
    // `configure()`.
    CY_CHECK_EQ(ground.system.sky_term().zenith.x, seam.term().zenith.x);
    CY_CHECK_EQ(ground.system.sky_term().horizon.y, seam.term().horizon.y);

    (void)ground.system.update(ground.context(5));
    const f32 with_atmosphere = ground_indirect(ground.system);
    CY_TEST_MESSAGE("ground indirect: placeholder ", with_placeholder, ", atmosphere ",
                    with_atmosphere);
    // THE PICTURE MOVED. A composition point that installs a term the resolve never reads would
    // leave these two identical, and would satisfy every assertion above it.
    CY_CHECK(std::fabs(with_atmosphere - with_placeholder) > 1e-4F);

    // --- And it is the ATMOSPHERE's, not a plausible constant --------------------------------------
    //
    // `sky::sky_irradiance` integrates the full atmosphere for an upward-facing surface and knows
    // nothing about the gradient fit. A term that carried the atmosphere's name on a constant would
    // fail this at the second elevation, because the constant cannot change when the sun does.
    const cy::rendering::sky::Atmosphere air = cy::rendering::sky::earth_atmosphere();
    const Vec3 view = cy::rendering::sky::ground_position(air, 0.0F);
    const auto agreement = [&](f32 elevation) {
        const Vec3 direction = sun_at(elevation);
        CY_REQUIRE(seam.set_atmosphere(air).has_value());
        (void)seam.update(frame_at(direction, 6), ground.system);
        const f32 fitted = magnitude(term_irradiance(seam.term(), kUp));
        const f32 reference = magnitude(cy::rendering::sky::sky_irradiance(air, view, direction, kUp));
        CY_TEST_MESSAGE("sun at ", elevation, " rad: fitted ", fitted, " lux, atmosphere ",
                        reference, " lux");
        CY_CHECK_GT(reference, 0.0F);
        CY_CHECK_LT(std::fabs(fitted - reference) / reference, 0.35F);
        return reference;
    };
    const f32 high = agreement(1.2F);
    const f32 low = agreement(0.12F);
    // The two elevations must be genuinely different skies, or the agreement above is one number
    // checked twice.
    CY_CHECK_GT(high / low, 1.5F);
}

CY_TEST_CASE("a moving sun is incremental and bounded against a full recomputation") {
    // THE REQUIREMENT'S SECOND HALF, WHICH AN ADAPTER DOES NOT DO: "sky-derived illumination SHALL
    // be updated incrementally and within the illumination budget, and the invalidation this
    // triggers SHALL be measured against a full recomputation rather than assumed smaller."
    //
    // The clock is a twenty-minute day at sixty frames a second, which is `TimeOfDay`'s own default
    // and the cadence a real sun moves at. The sun crosses the refit threshold roughly every forty
    // frames at that rate; a test that rotated the sun by a degree a frame would refit every frame
    // and would be measuring a stepping sun rather than a continuous one.
    //
    // ANCHORED AT SUNRISE, which is where the sky changes fastest and therefore where an
    // "incremental" claim is hardest rather than easiest. A take at noon would refit as rarely and
    // would move the irradiance by almost nothing, which would satisfy the step bound by having no
    // steps to bound.
    constexpr u32 kFrames = 1400;
    constexpr f32 kStep = 1.0F / 60.0F;
    /// The illumination system is advanced every fourth frame. Invalidation records accumulate in
    /// the GI scene and are serviced once each whenever `update()` next runs, so the counts below
    /// are the same counts a per-frame renderer would see — and a thousand probe gathers inside a
    /// one-second CPU budget are not the thing being measured.
    constexpr u32 kFramesPerIllumination = 4;

    cy::rendering::sky::TimeOfDay clock;
    clock.seconds_per_day = 1200.0F;
    clock.fraction = 0.27F;
    const cy::rendering::sky::CelestialModel model;

    Ground incremental;
    incremental.run(4);
    SkyIllumination seam;
    SkyIlluminationSettings levers;
    // The fit's direction count is the seam's one budget lever, moved here to what an integration
    // budget affords. It changes the fit's cost and nothing about when a fit happens, which is what
    // this case is measuring.
    levers.fit_samples = 12;
    CY_REQUIRE(seam.configure(levers).has_value());
    CY_REQUIRE(seam.set_atmosphere(cy::rendering::sky::earth_atmosphere()).has_value());

    u32 invalidations = 0;
    u32 probes_invalidated = 0;
    u32 bricks_invalidated = 0;
    f32 worst_step = 0.0F;
    f32 first = 0.0F;
    f32 last = 0.0F;
    cy::rendering::sky::TimeOfDay running = clock;
    for (u32 index = 0; index < kFrames; ++index) {
        const auto state = cy::rendering::sky::solve_celestial(model, running);
        const SkyIlluminationReport report =
            seam.update(frame_at(state.sun.direction, index + 4), incremental.system);
        worst_step = cy::math::max(worst_step, report.irradiance_step);
        if (index % kFramesPerIllumination == 0) {
            const IlluminationFrameReport frame =
                incremental.system.update(incremental.context(index + 4));
            invalidations += frame.invalidations_serviced;
            probes_invalidated += frame.invalidated_probes;
            bricks_invalidated += frame.invalidated_field_bricks;
        }
        const f32 irradiance = magnitude(term_irradiance(seam.term(), kUp));
        if (index == 0) {
            first = irradiance;
        }
        last = irradiance;
        cy::rendering::sky::advance_time_of_day(running, kStep);
    }

    const SkyIlluminationReport& totals = seam.last_report();
    CY_TEST_MESSAGE("over ", kFrames, " frames of a moving sun: ", totals.fits, " fits, ",
                    totals.reuses, " reuses, ", totals.directions_integrated,
                    " directions integrated, ", probes_invalidated, " probes and ",
                    bricks_invalidated, " field bricks invalidated; worst irradiance step ",
                    worst_step);

    // --- INCREMENTAL. Fewer fits than frames, and by a margin rather than by one ------------------
    CY_CHECK_GT(totals.fits, 0U);
    CY_CHECK_LT(totals.fits, kFrames / 4U);
    CY_CHECK_EQ(totals.fits + totals.reuses, kFrames);

    // --- ONLY THE ILLUMINATION THAT DEPENDS ON IT ------------------------------------------------
    //
    // A sun that rotated moved no geometry, so the sparse distance field — a representation of where
    // surfaces ARE — cannot have been invalidated by it. `InvalidationCause::SkyChanged` is the one
    // cause `IlluminationSystem::service_invalidations` skips the field for, and this is the number
    // that says the distinction is serviced rather than declared.
    CY_CHECK_EQ(bricks_invalidated, 0U);
    CY_CHECK_GT(invalidations, 0U);

    // --- NO VISIBLE STEP -------------------------------------------------------------------------
    //
    // The threshold is half the sun's own diameter, so the irradiance a surface receives may not
    // jump across a refit. Five per cent is the tolerance; the day's whole travel is the control,
    // because a term that never changed would satisfy the step bound perfectly.
    CY_CHECK_LT(worst_step, 0.05F);
    CY_TEST_MESSAGE("upward irradiance moved from ", first, " to ", last, " lux across the take");
    const f32 travel = std::fabs(last - first) / cy::math::max(first, last);
    CY_CHECK_GT(travel, 0.05F);
    // AND THE STEP BOUND IS TIGHT RELATIVE TO THE TRAVEL, which is the claim that survives a change
    // of scene: the sky moved by more than an order of magnitude further than any single refit
    // moved it, so the continuity is a property of the threshold rather than of a short take.
    CY_CHECK_GT(travel, 10.0F * worst_step);

    // --- THE NUMBER TO BEAT: A FULL RECOMPUTATION -------------------------------------------------
    //
    // What installing a new sky costs WITHOUT this seam is `IlluminationSystem::configure()`, which
    // is the only other way to change `IlluminationSettings::sky`: the clipmap is rebuilt, the probe
    // cache is discarded and the acceleration service is reconstructed. It is measured here rather
    // than asserted — eight frames of it, so the comparison is per-frame work and not a guess.
    constexpr u32 kFullFrames = 8;
    Ground full;
    full.run(4);
    u32 full_bricks = 0;
    u32 full_probes_created = 0;
    for (u32 index = 0; index < kFullFrames; ++index) {
        auto settings = sky_illumination_support::ground_settings();
        settings.sky = seam.term();
        CY_REQUIRE(full.system.configure(settings).has_value());
        CY_REQUIRE(full.system.field()
                       .place(1, full.field.asset(),
                              cy::Mat4::from_translation(
                                  Vec3{0.0F, -sky_illumination_support::kGroundHalfY, 0.0F}))
                       .has_value());
        const IlluminationFrameReport frame = full.system.update(full.context(index + 4));
        full_bricks += frame.field.bricks_solved;
        full_probes_created += frame.placement.probes_created;
    }
    const f32 full_per_frame = static_cast<f32>(full_bricks) / static_cast<f32>(kFullFrames);
    CY_TEST_MESSAGE("a full recomputation re-solves ", full_per_frame,
                    " field bricks per frame and re-creates ",
                    static_cast<f32>(full_probes_created) / static_cast<f32>(kFullFrames),
                    " probes; the seam re-solves 0 and re-creates 0");
    CY_CHECK_GT(full_bricks, 0U);
    // The comparison, as one inequality: the whole take cost the seam no brick at all, and a single
    // frame of the alternative costs a rebuild.
    CY_CHECK_LT(static_cast<f32>(bricks_invalidated), full_per_frame);
}

CY_TEST_CASE("cloud shadow reaches illumination through the coarse field") {
    // `atmosphere-sky-and-clouds`: the coarse cloud shadow field SHALL be "consumed by terrain,
    // foliage, water, and illumination". This module is the fourth consumer and it reads through
    // `CloudShadowField::sample` — the static function the other three call — for the reason
    // `m10:sky-field-round-trip` records: a consumer that picks its own sampling path is a consumer
    // the field's own declared defect cannot break.
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    cy::world::PartitionConfig partition;
    partition.partition = 1;
    partition.base_cell_size = 128.0F;
    partition.levels = 3;
    partition.level_ratio = 4;

    cy::environment::FieldRegistry registry(allocator);
    cy::rendering::sky::CloudShadowField shadow;
    cy::rendering::sky::CloudShadowQuality quality;
    quality.regional_cell_metres = 128.0F;
    quality.macro_cell_metres = 1024.0F;
    quality.radius_metres = 512.0F;
    quality.steps = 8;
    CY_REQUIRE(shadow.attach(registry, quality));
    CY_REQUIRE(cy::rendering::sky::CloudShadowField::declare_consumers(registry));
    cy::environment::FieldStore store(allocator, registry, partition);

    cy::rendering::sky::CloudWeatherMap map;
    CY_REQUIRE(map.configure(16, 1000.0F));
    CY_REQUIRE(map.generate(0x5ADE5ULL, 0.85F, 0.6F));
    cy::rendering::sky::CloudField clouds;
    clouds.map = &map;
    clouds.layers = cy::rendering::sky::default_cloud_layers();
    cy::rendering::sky::CloudWeatherState weather;
    weather.humidity = 0.85F;
    weather.storm_intensity = 0.6F;
    cy::rendering::sky::drive_cloud_layers(weather, clouds.layers);

    const Vec3 sun = sun_at(0.9F);
    const cy::world::WorldVec3d centre{0.0, 0.0, 0.0};
    const auto wrote = shadow.update(store, clouds, sun, 0.0, centre, 1.0F);
    CY_REQUIRE(wrote);
    CY_REQUIRE(wrote.value());
    CY_TEST_MESSAGE("cloud shadow field: darkest ", shadow.stats().darkest, ", mean ",
                    shadow.stats().mean, ", brightest ", shadow.stats().brightest);
    // The field has to BE a shadow before a consumer of it can mean anything. A field that is
    // uniformly one is the state `m10:sky-field-round-trip` was declared against.
    CY_REQUIRE(shadow.stats().darkest < 0.999F);

    Ground ground;
    ground.run(4);
    SkyIllumination seam;
    CY_REQUIRE(seam.configure(SkyIlluminationSettings{}).has_value());
    CY_REQUIRE(seam.set_atmosphere(cy::rendering::sky::earth_atmosphere()).has_value());

    // Unclouded first: the same seam, the same sun, no field attached.
    const SkyIlluminationReport clear = seam.update(frame_at(sun, 4), ground.system);
    CY_CHECK_FALSE(clear.cloud_field_read);
    CY_CHECK_EQ(clear.cloud_transmittance, 1.0F);
    CY_REQUIRE(clear.sun_from_atmosphere);
    const f32 unclouded_sun = clear.sun.intensity;
    CY_CHECK_GT(unclouded_sun, 0.0F);

    // A DARK CELL, found rather than assumed: the field is a map of the weather and the point under
    // the thickest part of it is where a consumer can be seen to read something.
    cy::world::WorldVec3d darkest{0.0, 0.0, 0.0};
    f32 darkest_value = 1.0F;
    for (cy::i32 z = -3; z <= 3; ++z) {
        for (cy::i32 x = -3; x <= 3; ++x) {
            const cy::world::WorldVec3d at{static_cast<double>(x) * 128.0, 0.0,
                                           static_cast<double>(z) * 128.0};
            const f32 value = cy::rendering::sky::CloudShadowField::sample(store, at);
            if (value < darkest_value) {
                darkest_value = value;
                darkest = at;
            }
        }
    }
    CY_REQUIRE(darkest_value < 0.999F);

    seam.set_cloud_shadow_source(&store);
    SkyIlluminationFrame clouded = frame_at(sun, 5);
    clouded.observer = darkest;
    const SkyIlluminationReport under = seam.update(clouded, ground.system);
    CY_CHECK(under.cloud_field_read);
    CY_TEST_MESSAGE("cloud transmittance at the darkest probed cell: ", under.cloud_transmittance,
                    "; sun ", unclouded_sun, " lux -> ", under.sun.intensity, " lux");
    CY_CHECK_LT(under.cloud_transmittance, 1.0F);
    CY_CHECK_EQ(under.cloud_transmittance, darkest_value);

    // AND IT REACHES ILLUMINATION. The sun the composition point hands the renderer is the
    // atmosphere's own `sun_illuminance` attenuated by the field, so the shadow is in the light the
    // GI system shades with rather than in a number a report carries.
    CY_CHECK_LT(under.sun.intensity, unclouded_sun);
    CY_CHECK_LT(std::fabs(under.sun.intensity - (unclouded_sun * darkest_value)),
                0.01F * unclouded_sun);

    // The direct term a surface receives with that sun, against the same surface with the
    // unclouded one. This is the consumption the requirement's word "illumination" means.
    std::vector<cy::rendering::gi::GiLight> clouded_lights{under.sun};
    std::vector<cy::rendering::gi::GiLight> clear_lights{clear.sun};
    const Vec3 shaded_under =
        cy::rendering::gi::shaded_direct({clouded_lights.data(), clouded_lights.size()},
                                         kQueryPoint, kUp, nullptr);
    const Vec3 shaded_clear = cy::rendering::gi::shaded_direct(
        {clear_lights.data(), clear_lights.size()}, kQueryPoint, kUp, nullptr);
    CY_TEST_MESSAGE("direct radiance on the ground: clear ", magnitude(shaded_clear), ", clouded ",
                    magnitude(shaded_under));
    CY_CHECK_LT(magnitude(shaded_under), magnitude(shaded_clear));

    // And the sky term itself is NOT attenuated: cloud cover scatters direct sunlight into the sky
    // rather than deleting it, so dimming both would take the same light out of the frame twice.
    CY_CHECK_EQ(under.provenance, SkyTermProvenance::PhysicalAtmosphere);
    CY_CHECK_EQ(seam.term().intensity, ground.system.sky_term().intensity);
}
