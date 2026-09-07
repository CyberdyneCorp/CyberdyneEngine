// The grading bake, at integration cost. Task 8.4.
//
// A 33³ table is 35,937 evaluations of the full reference grade, which is several milliseconds of
// honest work and belongs in the suite above `unit` rather than inside a one-millisecond budget.
// 33 is not negotiable down for the sake of the budget: it is the `.cube` standard and it is the
// size that ships, so a cheaper table here would be certifying something other than what runs.

#include <cy/test/test.h>

#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/post/grading.h>

#include <cmath>

namespace {

using cy::rendering::apply_grading;
using cy::rendering::GradingSettings;
using cy::rendering::sample_grading_lut;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

GradingSettings warm_and_contrasty() noexcept {
    GradingSettings settings;
    settings.temperature = 4200.0F;
    settings.tint = 0.1F;
    settings.contrast = 1.2F;
    settings.saturation = 0.85F;
    settings.gain = cy::Vec3{1.05F, 1.0F, 0.95F};
    settings.lift = cy::Vec3{0.01F, 0.0F, 0.02F};
    settings.shadows.gain = cy::Vec3{0.95F, 1.0F, 1.1F};
    settings.highlights.gain = cy::Vec3{1.05F, 1.0F, 0.95F};
    return settings;
}

}  // namespace

CY_TEST_CASE("the baked lookup table reproduces the reference grade") {
    const GradingSettings settings = warm_and_contrasty();
    constexpr cy::u32 kSize = 33;  // the .cube standard, and odd so a sample sits on neutral

    cy::Array<cy::Vec3> lut(allocator());
    CY_REQUIRE(lut.resize(static_cast<cy::usize>(kSize) * kSize * kSize).has_value());
    CY_REQUIRE(bake_grading_lut(settings, kSize, lut.span().data(), lut.size()));

    // The runtime's whole grading cost is one lookup, and it has to agree with the reference or the
    // grade a colourist approved is not the grade that ships.
    cy::f32 worst = 0.0F;
    for (cy::u32 step = 0; step < 40; ++step) {
        const cy::f32 t = static_cast<cy::f32>(step) / 39.0F;
        const cy::Vec3 colour{cy::rendering::log_decode(t),
                              cy::rendering::log_decode(cy::math::saturate(t + 0.13F)),
                              cy::rendering::log_decode(cy::math::saturate(t * 0.7F))};
        const cy::Vec3 reference = apply_grading(colour, settings);
        const cy::Vec3 sampled = sample_grading_lut(lut.span().data(), kSize, colour);
        const cy::f32 scale = cy::math::max(cy::math::max(reference.x, reference.y), 0.05F);
        worst = cy::math::max(worst, std::fabs(reference.x - sampled.x) / scale);
        worst = cy::math::max(worst, std::fabs(reference.y - sampled.y) / scale);
        worst = cy::math::max(worst, std::fabs(reference.z - sampled.z) / scale);
    }
    CY_CHECK_LT(worst, 0.06F);

    // A table too small for the size, and a degenerate size, are refused rather than written past.
    CY_CHECK_FALSE(bake_grading_lut(settings, kSize, lut.span().data(), 10));
    CY_CHECK_FALSE(bake_grading_lut(settings, 1, lut.span().data(), lut.size()));
    CY_CHECK_FALSE(bake_grading_lut(settings, kSize, nullptr, lut.size()));
    // Sampling a table that is not there returns the input rather than reading it.
    CY_CHECK_EQ(sample_grading_lut(nullptr, kSize, cy::Vec3{1.0F, 1.0F, 1.0F}).x, 1.0F);
}
