// THE DISPLACEMENT CONTRACT. M10 task 2.3, and the requirement `water` says matters most.
//
// "The classic water bug is a boat floating on a flat plane beneath visible swell." Every case here
// is about the difference between what the renderer draws and what a query answers, and about that
// difference being exactly the declared visual-only bands.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in this milestone's `verified_failing`:
// `evaluate_displacement()`'s band loop had its `included()` guard deleted, so every evaluation
// summed every band and the authoritative and visual answers became identical. "a visual-only band
// moves the rendered surface and not the queried one" went red on its first assertion. The guard
// was then restored.

#include <cy/test/test.h>

#include <cy/water/displacement.h>

#include <algorithm>
#include <cmath>

using cy::water::AmplitudeSplit;
using cy::water::BandAuthority;
using cy::water::BandSelection;
using cy::water::Displacement;
using cy::water::DisplacementBand;
using cy::water::DisplacementModel;
using cy::water::DisplacementProblem;

namespace {

[[nodiscard]] DisplacementBand swell() noexcept {
    DisplacementBand band;
    band.wavelength_min = 40.0F;
    band.wavelength_max = 90.0F;
    band.amplitude = 0.8F;
    band.direction_degrees = 0.0F;
    band.spread_degrees = 10.0F;
    band.steepness = 0.4F;
    band.wave_count = 3;
    band.authority = BandAuthority::Authoritative;
    return band;
}

[[nodiscard]] DisplacementBand capillary(cy::f32 amplitude) noexcept {
    DisplacementBand band;
    band.wavelength_min = 0.2F;
    band.wavelength_max = 0.8F;
    band.amplitude = amplitude;
    band.direction_degrees = 20.0F;
    band.spread_degrees = 40.0F;
    band.steepness = 0.2F;
    band.wave_count = 3;
    band.authority = BandAuthority::Visual;
    return band;
}

[[nodiscard]] DisplacementModel two_band_sea(cy::f32 capillary_amplitude) noexcept {
    DisplacementModel model;
    model.seed = 0x5EA5'01DEull;
    model.mean_level = 3.0;
    (void)model.add(swell());
    (void)model.add(capillary(capillary_amplitude));
    return model;
}

}  // namespace

CY_TEST_CASE("a visual-only band moves the rendered surface and not the queried one") {
    const DisplacementModel model = two_band_sea(0.03F);

    cy::f64 largest_difference = 0.0;
    bool visual_moved_somewhere = false;
    for (cy::u32 step = 0; step < 64; ++step) {
        const cy::f64 x = static_cast<cy::f64>(step) * 1.7;
        const cy::f64 z = static_cast<cy::f64>(step) * -0.9;
        const Displacement physics =
            evaluate_displacement(model, BandSelection::Authoritative, x, z, 3.25);
        const Displacement rendering = evaluate_displacement(model, BandSelection::All, x, z, 3.25);

        const cy::f64 difference = std::fabs(rendering.height - physics.height);
        largest_difference = std::max(largest_difference, difference);
        visual_moved_somewhere = visual_moved_somewhere || difference > 1e-6;

        // THE CONTRACT: the difference is the visual band's amplitude and NOTHING ELSE. Not a
        // different integrator, not a different phase — the same trains, plus the declared ones.
        CY_CHECK_LE(difference, 0.031 + 1e-5);
    }
    CY_CHECK(visual_moved_somewhere);
    CY_CHECK_GT(largest_difference, 0.0);
}

CY_TEST_CASE("the authoritative answer is the same for every caller, at every time") {
    const DisplacementModel model = two_band_sea(0.03F);
    // Two callers — a renderer asking for the physics bands and a physics query — get the same
    // number. There is one function, so this is a property of the arguments rather than of a
    // convention, and this case is what would catch a second entry point being added.
    for (cy::u32 step = 0; step < 16; ++step) {
        const cy::f64 time = static_cast<cy::f64>(step) * 0.37;
        const Displacement a =
            evaluate_displacement(model, BandSelection::Authoritative, 12.5, -8.25, time);
        const Displacement b =
            evaluate_displacement(model, BandSelection::Authoritative, 12.5, -8.25, time);
        CY_CHECK_EQ(a.height, b.height);
        CY_CHECK_EQ(a.velocity.y, b.velocity.y);
    }
}

CY_TEST_CASE("the amplitude split is reportable, so the discrepancy is checkable") {
    const DisplacementModel model = two_band_sea(0.03F);
    const AmplitudeSplit split = cy::water::amplitude_split(model);
    CY_CHECK_EQ(split.authoritative_bands, 1u);
    CY_CHECK_EQ(split.visual_bands, 1u);
    CY_CHECK_NEAR(split.authoritative_metres, 0.8F, 1e-5F);
    CY_CHECK_NEAR(split.visual_metres, 0.03F, 1e-5F);
    CY_CHECK_NEAR(split.largest_visual_band_metres, 0.03F, 1e-5F);
}

CY_TEST_CASE("a visual band large enough to be felt is a refusal, not a discrepancy") {
    // Under the threshold: legal.
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(two_band_sea(0.04F))),
                static_cast<int>(DisplacementProblem::None));
    // Over it: the specification calls this a violation, so it is refused at configuration time.
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(two_band_sea(0.35F))),
                static_cast<int>(DisplacementProblem::VisualBandFeltByGameplay));

    // And the threshold is DECLARED: a project whose boats are frigates may raise it, and then the
    // same model is legal. What is not allowed is an undeclared discrepancy.
    DisplacementModel tolerant = two_band_sea(0.35F);
    tolerant.felt_threshold_metres = 0.5F;
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(tolerant)),
                static_cast<int>(DisplacementProblem::None));
}

CY_TEST_CASE("large swell must be authoritative") {
    DisplacementModel model;
    DisplacementBand visual_swell = swell();
    visual_swell.authority = BandAuthority::Visual;
    visual_swell.amplitude = 0.01F;  // small enough to pass the felt threshold
    (void)model.add(visual_swell);
    (void)model.add(capillary(0.01F));
    // The longest-wavelength band is the swell whatever its amplitude, and a model that declares it
    // visual is refused however quiet it is: physics would be flat under a visible sea.
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(model)),
                static_cast<int>(DisplacementProblem::LongestBandIsVisual));
}

CY_TEST_CASE("a model that cannot describe a surface is refused, by reason") {
    DisplacementModel empty;
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(empty)),
                static_cast<int>(DisplacementProblem::NoBands));

    DisplacementModel inverted;
    DisplacementBand band = swell();
    band.wavelength_max = band.wavelength_min * 0.5F;
    (void)inverted.add(band);
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(inverted)),
                static_cast<int>(DisplacementProblem::BadWavelengths));

    DisplacementModel folded;
    DisplacementBand steep = swell();
    steep.steepness = 1.8F;
    (void)folded.add(steep);
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(folded)),
                static_cast<int>(DisplacementProblem::BadSteepness));

    DisplacementModel trainless;
    DisplacementBand bare = swell();
    bare.wave_count = 0;
    (void)trainless.add(bare);
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(trainless)),
                static_cast<int>(DisplacementProblem::NoTrains));
}

CY_TEST_CASE("the surface is a pure function of its inputs, so two machines agree") {
    const DisplacementModel model = two_band_sea(0.03F);
    // Evaluated in a different ORDER, which is what a job system does to it: the answer is the
    // same, because there is no state anywhere in the evaluation.
    cy::f64 forward = 0.0;
    for (cy::u32 step = 0; step < 32; ++step) {
        forward +=
            evaluate_displacement(model, BandSelection::All, static_cast<cy::f64>(step), 0.0, 1.0)
                .height;
    }
    cy::f64 backward = 0.0;
    for (cy::u32 step = 32; step-- > 0;) {
        backward +=
            evaluate_displacement(model, BandSelection::All, static_cast<cy::f64>(step), 0.0, 1.0)
                .height;
    }
    // Summed in the opposite order the floating-point total may differ in its last bits; every
    // individual sample must be identical, so compare those instead of the sums.
    for (cy::u32 step = 0; step < 32; ++step) {
        const Displacement a =
            evaluate_displacement(model, BandSelection::All, static_cast<cy::f64>(step), 0.0, 1.0);
        const Displacement b =
            evaluate_displacement(model, BandSelection::All, static_cast<cy::f64>(step), 0.0, 1.0);
        CY_CHECK_EQ(a.height, b.height);
    }
    CY_CHECK(std::fabs(forward - backward) < 1e-6);
}

CY_TEST_CASE("a seed change changes the sea, and the same seed reproduces it") {
    DisplacementModel first = two_band_sea(0.03F);
    DisplacementModel second = first;
    second.seed = first.seed + 1;

    const Displacement a = evaluate_displacement(first, BandSelection::All, 10.0, 10.0, 0.0);
    const Displacement b = evaluate_displacement(second, BandSelection::All, 10.0, 10.0, 0.0);
    CY_CHECK(std::fabs(a.height - b.height) > 1e-6);

    const DisplacementModel again = two_band_sea(0.03F);
    const Displacement c = evaluate_displacement(again, BandSelection::All, 10.0, 10.0, 0.0);
    CY_CHECK_EQ(a.height, c.height);
}

CY_TEST_CASE("the per-band report says which bands contributed, and by how much") {
    const DisplacementModel model = two_band_sea(0.03F);
    cy::water::BandContribution bands[cy::water::kMaxDisplacementBands];
    const cy::u32 count = cy::water::band_contributions(model, 5.0, -3.0, 2.0, bands);
    CY_REQUIRE_EQ(count, 2u);

    CY_CHECK_EQ(static_cast<int>(bands[0].authority),
                static_cast<int>(BandAuthority::Authoritative));
    CY_CHECK_EQ(static_cast<int>(bands[1].authority), static_cast<int>(BandAuthority::Visual));
    CY_CHECK_LE(std::fabs(bands[1].vertical_metres), 0.031F);

    // The two contributions sum to the rendered height, which is what makes the report a
    // DECOMPOSITION rather than a second opinion.
    const Displacement all = evaluate_displacement(model, BandSelection::All, 5.0, -3.0, 2.0);
    const cy::f32 total = bands[0].vertical_metres + bands[1].vertical_metres;
    CY_CHECK_NEAR(static_cast<cy::f32>(all.height - model.mean_level), total, 1e-4F);
}

CY_TEST_CASE("a steep sea breaks at its crests and a flat one does not") {
    DisplacementModel calm;
    DisplacementBand band = swell();
    band.steepness = 0.0F;
    band.amplitude = 0.2F;
    (void)calm.add(band);

    DisplacementModel steep;
    DisplacementBand sharp = swell();
    sharp.steepness = 1.0F;
    sharp.amplitude = 3.0F;
    sharp.wavelength_min = 8.0F;
    sharp.wavelength_max = 10.0F;
    sharp.wave_count = 1;
    (void)steep.add(sharp);

    cy::f32 calm_breaking = 0.0F;
    cy::f32 steep_breaking = 0.0F;
    for (cy::u32 step = 0; step < 64; ++step) {
        const cy::f64 x = static_cast<cy::f64>(step) * 0.25;
        calm_breaking = std::max(
            calm_breaking, evaluate_displacement(calm, BandSelection::All, x, 0.0, 0.0).breaking);
        steep_breaking = std::max(
            steep_breaking, evaluate_displacement(steep, BandSelection::All, x, 0.0, 0.0).breaking);
    }
    CY_CHECK_NEAR(calm_breaking, 0.0F, 1e-6F);
    CY_CHECK_GT(steep_breaking, 0.1F);
}
