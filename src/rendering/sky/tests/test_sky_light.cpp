// THE CRITERION OF TASK 10.4: "an analytic sky sufficient for GI's sky term", measured.
//
// The gradient `rendering-global-illumination` consumes has three colours. The question this file
// answers is not whether those three colours are plausible — it is whether the IRRADIANCE the
// gradient delivers matches the irradiance the full atmosphere delivers, over a day, for surfaces
// facing in several directions. A gradient that agreed with the sky only at the zenith would pass a
// spot check and be wrong for every surface that is not facing straight up.
//
// Integration rather than unit: each irradiance integral is 2,048 directions and each direction is
// a ray march through the atmosphere. That is the cost of a reference.

#include <cy/test/test.h>

#include <cy/core/jobs/job_system.h>
#include <cy/core/math/math.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/scope.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/sky_light.h>

#include <cmath>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::sky::Atmosphere;
using cy::rendering::sky::CelestialModel;
using cy::rendering::sky::earth_atmosphere;
using cy::rendering::sky::fit_sky_gradient;
using cy::rendering::sky::ground_position;
using cy::rendering::sky::project_sky_irradiance;
using cy::rendering::sky::sky_irradiance;
using cy::rendering::sky::sky_irradiance_directions;
using cy::rendering::sky::sky_radiance;
using cy::rendering::sky::SkyGradient;
using cy::rendering::sky::SkyIrradianceSh;
using cy::rendering::sky::SkyTableQuality;
using cy::rendering::sky::SkyViewTable;
using cy::rendering::sky::solve_celestial;
using cy::rendering::sky::thin_dusty_atmosphere;
using cy::rendering::sky::TimeOfDay;

/// The irradiance a gradient delivers to a surface, by the same integration the reference uses. The
/// gradient is evaluated through its OWN `radiance()`, which is the function `gi::SkyTerm` mirrors
/// — so this measures the thing GI would actually see.
Vec3 gradient_irradiance(const SkyGradient& gradient, Vec3 normal, u32 steps) noexcept {
    const f32 weight = 4.0F * cy::math::kPi / static_cast<f32>(steps * steps * 2U);
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (u32 i = 0; i < steps; ++i) {
        const f32 cos_theta =
            1.0F - (2.0F * (static_cast<f32>(i) + 0.5F) / static_cast<f32>(steps));
        const f32 sin_theta = std::sqrt(cy::math::max(0.0F, 1.0F - (cos_theta * cos_theta)));
        for (u32 j = 0; j < steps * 2U; ++j) {
            const f32 phi =
                2.0F * cy::math::kPi * (static_cast<f32>(j) + 0.5F) / static_cast<f32>(steps * 2U);
            const Vec3 direction{sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi)};
            const f32 cosine = dot(direction, normal);
            if (cosine > 0.0F) {
                total = total + gradient.radiance(direction) * (cosine * weight);
            }
        }
    }
    return total;
}

f32 relative_difference(Vec3 a, Vec3 b) noexcept {
    const f32 scale = cy::math::max(a.x + a.y + a.z, 1.0e-6F);
    return (std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z)) / scale;
}

}  // namespace

CY_TEST_CASE("sky light: the gradient GI consumes delivers the atmosphere's own irradiance") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 0.0F);

    // Four sun elevations across a day, and three surface orientations at each: straight up, a wall
    // facing the sun, and a wall facing away. The last is the one a zenith-only fit gets wrong.
    const Vec3 normals[3] = {Vec3{0.0F, 1.0F, 0.0F}, normalize(Vec3{1.0F, 0.2F, 0.0F}),
                             normalize(Vec3{-1.0F, 0.2F, 0.0F})};

    f32 worst = 0.0F;
    f32 sum = 0.0F;
    u32 count = 0;
    for (const f32 elevation : {0.9F, 0.5F, 0.2F, 0.05F}) {
        const Vec3 sun = normalize(
            Vec3{std::sqrt(cy::math::max(0.0F, 1.0F - (elevation * elevation))), elevation, 0.0F});
        const SkyGradient gradient = fit_sky_gradient(earth, eye, sun, 16);
        for (const Vec3& normal : normals) {
            const Vec3 reference = sky_irradiance(earth, eye, sun, normal, 16);
            const Vec3 fitted = gradient_irradiance(gradient, normal, 16);
            const f32 error = relative_difference(reference, fitted);
            worst = cy::math::max(worst, error);
            sum += error;
            ++count;
        }
    }

    const f32 mean = sum / static_cast<f32>(count);
    CY_TEST_MESSAGE("gradient vs atmosphere: mean relative irradiance difference ", mean * 100.0F,
                    "% over ", count, " sun and surface combinations, worst ", worst * 100.0F, "%");

    // THIS TOLERANCE IS WHAT "SUFFICIENT FOR GI'S SKY TERM" MEANS HERE, and it is read off the
    // measurement rather than chosen: 12.4% mean and 17.4% worst. A three-colour gradient cannot
    // reproduce a sky that is bright on one side and dark on the other, and the error is largest at
    // a low sun where exactly that is true. What it buys is an ambient term of the right magnitude
    // and the right colour that costs three lerps instead of a ray march.
    //
    // The MEAN leads and the worst is beside it, per M7's own artefact rule.
    CY_CHECK_LT(mean, 0.18F);
    CY_CHECK_LT(worst, 0.25F);
}

CY_TEST_CASE("sky light: the horizon colour is solved for, and sampling it instead is worse") {
    // The claim `fit_sky_gradient` makes in its own comment, measured rather than asserted: a
    // gradient whose horizon is a SAMPLE of the sky delivers materially less light than one whose
    // horizon is solved for, because most of a hemisphere's solid angle is nowhere near either
    // sample direction.
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 0.0F);
    const Vec3 sun = normalize(Vec3{0.5F, 0.6F, 0.0F});
    const Vec3 up{0.0F, 1.0F, 0.0F};

    const SkyGradient solved = fit_sky_gradient(earth, eye, sun, 16);

    SkyGradient sampled = solved;
    sampled.horizon = sky_radiance(earth, eye, normalize(Vec3{1.0F, 0.02F, 0.0F}), sun, 24);

    // Compared over the SAME set the fit is solved against, because a comparison at one
    // orientation is exactly the mistake the fit was changed to stop making.
    const Vec3 normals[3] = {up, normalize(Vec3{1.0F, 0.25F, 0.0F}),
                             normalize(Vec3{-1.0F, 0.25F, 0.0F})};
    f32 solved_error = 0.0F;
    f32 sampled_error = 0.0F;
    for (const Vec3& normal : normals) {
        const Vec3 reference = sky_irradiance(earth, eye, sun, normal, 16);
        solved_error += relative_difference(reference, gradient_irradiance(solved, normal, 16));
        sampled_error += relative_difference(reference, gradient_irradiance(sampled, normal, 16));
    }
    solved_error /= 3.0F;
    sampled_error /= 3.0F;

    CY_TEST_MESSAGE("mean irradiance over three orientations: solved horizon ",
                    solved_error * 100.0F, "% off, sampled horizon ", sampled_error * 100.0F,
                    "% off");
    CY_CHECK_LT(solved_error, sampled_error);
}

CY_TEST_CASE("sky light: the spherical-harmonic projection reconstructs the diffuse term") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 0.0F);
    const Vec3 sun = normalize(Vec3{0.4F, 0.55F, 0.0F});

    const SkyIrradianceSh sh = project_sky_irradiance(earth, eye, sun, 20);

    f32 worst = 0.0F;
    for (const Vec3& normal :
         {Vec3{0.0F, 1.0F, 0.0F}, normalize(Vec3{1.0F, 0.3F, 0.0F}),
          normalize(Vec3{-1.0F, 0.3F, 0.0F}), normalize(Vec3{0.0F, 0.3F, 1.0F})}) {
        const Vec3 reference = sky_irradiance(earth, eye, sun, normal, 16);
        worst = cy::math::max(worst, relative_difference(reference, sh.irradiance(normal)));
    }
    CY_TEST_MESSAGE("SH-9 irradiance: worst relative difference ", worst * 100.0F,
                    "% over four orientations");
    // Nine coefficients reconstruct a cosine-convolved sky closely; the residual here is the
    // quadrature's, not the basis's.
    CY_CHECK_LT(worst, 0.12F);

    // AND IT NEVER GOES NEGATIVE. A nine-coefficient reconstruction of a sky with a bright sun
    // rings, and negative irradiance reaches a shader as a subtraction — a black rim on the
    // shadowed side of everything. The clamp is in `irradiance()` and this is what says so.
    for (const Vec3& normal : {Vec3{0.0F, -1.0F, 0.0F}, normalize(Vec3{-1.0F, -0.6F, 0.0F})}) {
        const Vec3 value = sh.irradiance(normal);
        CY_CHECK_GE(value.x, 0.0F);
        CY_CHECK_GE(value.y, 0.0F);
        CY_CHECK_GE(value.z, 0.0F);
    }
}

CY_TEST_CASE(
    "sky light: the table is a lookup, is rebuilt only when something moved, and reports") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 0.0F);
    SkyViewTable table;
    CY_REQUIRE(table.configure(SkyTableQuality::Low));
    CY_CHECK_EQ(table.quality(), SkyTableQuality::Low);
    CY_CHECK_FALSE(table.built());
    CY_CHECK_EQ(table.sample(Vec3{0.0F, 1.0F, 0.0F}).x, 0.0F);

    const Vec3 sun = normalize(Vec3{0.3F, 0.7F, 0.0F});
    const auto first = table.update(earth, eye, sun);
    CY_REQUIRE(first);
    CY_CHECK(first.value());
    CY_CHECK_EQ(table.stats().full_rebuilds, 1U);
    CY_CHECK_EQ(table.stats().directions_integrated, 32U * 16U);

    // "Tables SHALL be regenerated only when the parameters they depend on change." A sun driven
    // from a clock is never twice the same float, so the comparison is by ANGLE — otherwise the
    // table is rebuilt every frame and is not a table.
    const Vec3 nudged = normalize(sun + Vec3{1.0e-5F, 0.0F, 0.0F});
    const auto second = table.update(earth, eye, nudged);
    CY_REQUIRE(second);
    CY_CHECK_FALSE(second.value());
    CY_CHECK_EQ(table.stats().full_rebuilds, 1U);
    CY_CHECK_EQ(table.stats().reuses, 1U);

    // Move it past the threshold and it rebuilds. **THE REBUILD IS FULL, NOT INCREMENTAL** — the
    // requirement asks for incremental regeneration as the sun moves and this tier does not do it,
    // which is why `full_rebuilds` is a counter a reader can see rather than a silence.
    const auto third = table.update(earth, eye, normalize(Vec3{0.4F, 0.7F, 0.0F}));
    CY_REQUIRE(third);
    CY_CHECK(third.value());
    CY_CHECK_EQ(table.stats().full_rebuilds, 2U);

    // A composition change rebuilds too, at the same sun.
    Atmosphere dustier = earth;
    dustier.mie_scattering *= 2.0F;
    const auto fourth = table.update(dustier, eye, normalize(Vec3{0.4F, 0.7F, 0.0F}));
    CY_REQUIRE(fourth);
    CY_CHECK(fourth.value());
    CY_CHECK_EQ(table.stats().full_rebuilds, 3U);
}

CY_TEST_CASE("sky light: the table agrees with the model it was built from, and has no seam") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 0.0F);
    const Vec3 sun = normalize(Vec3{0.3F, 0.6F, 0.0F});
    SkyViewTable table;
    CY_REQUIRE(table.configure(SkyTableQuality::High));
    CY_REQUIRE(table.update(earth, eye, sun));

    f32 worst = 0.0F;
    for (const f32 elevation : {0.05F, 0.3F, 0.7F, 0.95F}) {
        for (const f32 azimuth : {0.4F, 2.1F, 4.7F}) {
            const f32 horizontal = std::sqrt(cy::math::max(0.0F, 1.0F - (elevation * elevation)));
            const Vec3 direction{horizontal * std::cos(azimuth), elevation,
                                 horizontal * std::sin(azimuth)};
            const Vec3 exact = sky_radiance(earth, eye, direction, sun, 16);
            worst = cy::math::max(worst, relative_difference(exact, table.sample(direction)));
        }
    }
    CY_TEST_MESSAGE("sky view table at high quality: worst relative difference from the model ",
                    worst * 100.0F, "%");
    CY_CHECK_LT(worst, 0.10F);

    // THE AZIMUTH WRAPS. A table whose last column does not blend into its first shows a seam
    // straight up the sky, and it is invisible until somebody turns around.
    const f32 epsilon = 1.0e-4F;
    const Vec3 before{std::cos(-epsilon), 0.4F, std::sin(-epsilon)};
    const Vec3 after{std::cos(epsilon), 0.4F, std::sin(epsilon)};
    CY_CHECK_LT(
        relative_difference(table.sample(normalize(before)), table.sample(normalize(after))),
        0.01F);
}

CY_TEST_CASE("sky light: a thin dusty planet's GI term follows from its own atmosphere") {
    // The same fit, on a planet that is not Earth: the gradient must still deliver that planet's
    // irradiance, or "sufficient for GI's sky term" is a statement about one parameter set.
    const Atmosphere dusty = thin_dusty_atmosphere();
    const Vec3 eye = ground_position(dusty, 0.0F);
    const Vec3 sun = normalize(Vec3{0.4F, 0.6F, 0.0F});
    const Vec3 up{0.0F, 1.0F, 0.0F};

    const SkyGradient gradient = fit_sky_gradient(dusty, eye, sun, 16);
    const Vec3 reference = sky_irradiance(dusty, eye, sun, up, 16);
    const f32 error = relative_difference(reference, gradient_irradiance(gradient, up, 16));
    CY_TEST_MESSAGE("thin dusty planet: upward irradiance ", error * 100.0F, "% off");
    // The same order as Earth's, which is the point: the fit is a property of the METHOD and not of
    // one parameter set. It is not smaller — a nearly uniform dusty sky has a horizon colour the
    // least-squares solution has to clamp, and `fit_sky_gradient` says so at the clamp.
    CY_CHECK_LT(error, 0.25F);

    // And its sky is not Earth's blue: the gradient's own colours say so, which is the whole point
    // of the term following from the coefficients.
    const SkyGradient earth_gradient =
        fit_sky_gradient(earth_atmosphere(), ground_position(earth_atmosphere(), 0.0F), sun, 16);
    const f32 dusty_blue = gradient.zenith.z / cy::math::max(gradient.zenith.x, 1.0e-9F);
    const f32 earth_blue =
        earth_gradient.zenith.z / cy::math::max(earth_gradient.zenith.x, 1.0e-9F);
    CY_CHECK_LT(dusty_blue, earth_blue);
}

CY_TEST_CASE("sky light: a day of sun positions produces a GI term that never goes negative") {
    // The end-to-end shape: the celestial model drives the sun, the atmosphere drives the sky, and
    // the gradient is what GI reads. Nothing in that chain may produce a negative colour, which is
    // the one failure that reaches a shader as a subtraction.
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 0.0F);
    CelestialModel model;
    TimeOfDay time;
    time.day_of_year = 172.0F;

    for (u32 step = 0; step < 12; ++step) {
        time.fraction = static_cast<f32>(step) / 12.0F;
        const auto state = solve_celestial(model, time);
        const SkyGradient gradient = fit_sky_gradient(earth, eye, state.sun.direction, 12);
        for (const Vec3 colour : {gradient.zenith, gradient.horizon, gradient.ground}) {
            CY_CHECK_GE(colour.x, 0.0F);
            CY_CHECK_GE(colour.y, 0.0F);
            CY_CHECK_GE(colour.z, 0.0F);
        }
    }
}

CY_TEST_CASE("sky light: the table and the irradiance integrated on workers are the serial bits") {
    // `FrameAssembly` rebuilds both every frame the sun or the eye moves, and `samples/10-world`
    // measured them at over half of its submit band on one thread, so the frame now lends them its
    // job system. What makes that safe is that the answer cannot depend on the schedule: every
    // direction is written to its own slot and the irradiance is summed in index order. EQUALITY,
    // not a tolerance — a parallel ambient that differed in the last bit would make a frame's
    // pixels depend on whether a job system was attached.
    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    config.task_slots_per_participant = 256;
    config.deque_capacity = 256;
    config.scratch_bytes_per_participant = cy::usize{64} * 1024;
    CY_REQUIRE(jobs.start(config).has_value());

    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 120.0F);
    cy::Array<Vec3> terms(cy::current_allocator());
    CY_REQUIRE(terms.resize(sky_irradiance_directions()).has_value());
    const cy::Span<Vec3> slots(terms.data(), terms.size());

    // A low sun and a high one, facing up and facing a wall: the lit half of the sphere moves with
    // the normal, which is what the index-order fold has to follow.
    const Vec3 suns[2] = {normalize(Vec3{0.9F, 0.12F, 0.1F}), normalize(Vec3{0.3F, 0.8F, 0.2F})};
    const Vec3 normals[2] = {Vec3{0.0F, 1.0F, 0.0F}, normalize(Vec3{-1.0F, 0.2F, 0.3F})};
    u32 compared = 0;
    for (const Vec3 sun : suns) {
        for (const Vec3 normal : normals) {
            const Vec3 serial = sky_irradiance(earth, eye, sun, normal);
            const auto parallel = sky_irradiance(earth, eye, sun, normal, 32, jobs, slots);
            CY_REQUIRE(parallel.has_value());
            CY_CHECK_GT(serial.y, 0.0F);
            CY_CHECK_EQ(parallel->x, serial.x);
            CY_CHECK_EQ(parallel->y, serial.y);
            CY_CHECK_EQ(parallel->z, serial.z);
            ++compared;
        }

        SkyViewTable serial_table;
        SkyViewTable parallel_table;
        CY_REQUIRE(serial_table.configure(SkyTableQuality::Low).has_value());
        CY_REQUIRE(parallel_table.configure(SkyTableQuality::Low).has_value());
        CY_REQUIRE(serial_table.update(earth, eye, sun).has_value());
        CY_REQUIRE(parallel_table.update(earth, eye, sun, &jobs).has_value());
        u32 differing = 0;
        // Every cell is read through the bilinear sampler at its own centre and between cells, so
        // a single differing cell anywhere in the table shows up.
        for (u32 row = 0; row < 33; ++row) {
            const f32 elevation = ((static_cast<f32>(row) / 32.0F) - 0.5F) * cy::math::kPi;
            for (u32 column = 0; column < 64; ++column) {
                const f32 azimuth = static_cast<f32>(column) / 64.0F * 2.0F * cy::math::kPi;
                const Vec3 direction{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                                     std::cos(elevation) * std::sin(azimuth)};
                const Vec3 a = serial_table.sample(direction);
                const Vec3 b = parallel_table.sample(direction);
                const bool same = a.x == b.x && a.y == b.y && a.z == b.z;
                differing += same ? 0U : 1U;
            }
        }
        CY_CHECK_EQ(differing, 0U);
    }
    CY_CHECK_EQ(compared, 4U);

    // A span too short for the sampler is refused rather than written past.
    const auto refused = sky_irradiance(earth, eye, suns[0], normals[0], 32, jobs,
                                        slots.first(sky_irradiance_directions() - 1U));
    CY_CHECK(!refused.has_value());
    jobs.shutdown();
}
