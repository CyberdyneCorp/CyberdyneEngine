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
#include <cy/rendering/post/lut.h>

#include "cube_fixtures.h"

#include <cmath>
#include <cstdio>

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

namespace {

struct TableError {
    cy::f32 worst = 0.0F;
    cy::Vec3 worst_at;
    cy::u32 over_one = 0;
    cy::f64 mean = 0.0;
};

/// The table the graded resolve binds, at `size`, against the grade evaluated directly — over
/// display-linear colours spread through the whole cube, measured in the 8-bit sRGB codes a viewer
/// sees. sRGB-uniform steps offset by 0.29 of a step, so the samples sit between lattice points.
TableError display_table_error(const cy::rendering::DisplayGrade& grade, cy::u32 size) {
    cy::Array<cy::Vec3> lut(allocator());
    CY_REQUIRE(lut.resize(static_cast<cy::usize>(size) * size * size).has_value());
    CY_REQUIRE(bake_display_lut(grade, size, lut.span().data(), lut.size()));
    constexpr cy::u32 kSteps = 24;
    const auto channel = [](cy::u32 index) noexcept {
        return cy::rendering::srgb_decode((static_cast<cy::f32>(index) + 0.29F) / kSteps);
    };
    const auto code = [](cy::f32 value) noexcept {
        return cy::rendering::srgb_encode(value) * 255.0F;
    };
    TableError out;
    cy::f64 total = 0.0;
    for (cy::u32 b = 0; b < kSteps; ++b) {
        for (cy::u32 g = 0; g < kSteps; ++g) {
            for (cy::u32 r = 0; r < kSteps; ++r) {
                const cy::Vec3 colour{channel(r), channel(g), channel(b)};
                const cy::Vec3 reference = cy::rendering::apply_display_grade(colour, grade);
                const cy::Vec3 sampled =
                    cy::rendering::sample_display_lut(lut.span().data(), size, colour);
                const cy::f32 error =
                    cy::math::max(std::fabs(code(reference.x) - code(sampled.x)),
                                  cy::math::max(std::fabs(code(reference.y) - code(sampled.y)),
                                                std::fabs(code(reference.z) - code(sampled.z))));
                if (error > out.worst) {
                    out.worst = error;
                    out.worst_at = colour;
                }
                out.over_one += error > 1.0F ? 1U : 0U;
                total += static_cast<cy::f64>(error);
            }
        }
    }
    out.mean = total / (kSteps * kSteps * kSteps);
    std::fprintf(stderr,
                 "display table %u^3: worst %.3f sRGB steps at (%.5f %.5f %.5f), %u of %u samples "
                 "over one step, mean %.4f\n",
                 size, static_cast<double>(out.worst), static_cast<double>(out.worst_at.x),
                 static_cast<double>(out.worst_at.y), static_cast<double>(out.worst_at.z),
                 out.over_one, kSteps * kSteps * kSteps, out.mean);
    return out;
}

}  // namespace

CY_TEST_CASE("the baked display table reproduces its reference grade, measured in 8-bit steps") {
    // A strong grade — warm, a fifth more contrast, saturation down, lift and wheels — through the
    // table the runtime binds. MEASURED at 33³: mean 0.84 of a step, worst 8.3 steps, at a colour
    // with one channel near black (0.934, 0.001, 0.124) where contrast and saturation crush that
    // channel through the clamp and the cell beside it does not. At 65³ the same sweep measures a
    // mean of 0.18 and a worst of 4.5: the error is the table's resolution, not a bias in the bake
    // — which is what the bounds below hold, with headroom for another host's libm.
    cy::rendering::DisplayGrade grade;
    grade.settings = warm_and_contrasty();
    const TableError shipping = display_table_error(grade, cy::rendering::kDisplayLutSize);
    CY_CHECK_LT(shipping.mean, 1.0);
    CY_CHECK_LT(shipping.worst, 9.0F);
    const TableError finer = display_table_error(grade, 65);
    CY_CHECK_LT(finer.mean, 0.25);
    CY_CHECK_LT(finer.worst, shipping.worst);
}

// --- The display table's known answers, at the size that ships (35,937 evaluations a bake) ---

namespace {

using cy::f32;
using cy::Vec3;
using cy::post_test::bake;
using cy::post_test::cube_text;
using cy::post_test::rotate_channels;
using cy::rendering::apply_display_grade;
using cy::rendering::CubeEncoding;
using cy::rendering::CubeLut;
using cy::rendering::display_lut_is_identity;
using cy::rendering::DisplayGrade;
using cy::rendering::kDisplayLutSize;
using cy::rendering::parse_cube_lut;
using cy::rendering::sample_display_lut;

}  // namespace

CY_TEST_CASE("a known LUT maps known colours to their expected values") {
    // In the file's linear encoding, a rotation of the channels: every output is known exactly, and
    // it is linear in the encoding, so the engine's baked table must reproduce it to the rounding
    // of the encode/decode pair — at the lattice AND between it.
    CubeLut rotation(allocator());
    CY_REQUIRE(parse_cube_lut(cube_text(2, rotate_channels), rotation).has_value());
    DisplayGrade grade;
    grade.cube = &rotation;
    grade.cube_encoding = CubeEncoding::Linear;
    const std::vector<Vec3> table = bake(grade);
    const Vec3 inputs[] = {Vec3{1.0F, 0.0F, 0.0F},    Vec3{0.0F, 1.0F, 0.0F},
                           Vec3{0.0F, 0.0F, 1.0F},    Vec3{0.2F, 0.5F, 0.8F},
                           Vec3{0.03F, 0.7F, 0.004F}, Vec3{1.0F, 1.0F, 1.0F}};
    for (const Vec3 input : inputs) {
        const Vec3 expected = rotate_channels(input);
        const Vec3 sampled = sample_display_lut(table.data(), kDisplayLutSize, input);
        CY_CHECK_NEAR(sampled.x, expected.x, 1e-4F + (expected.x * 1e-4F));
        CY_CHECK_NEAR(sampled.y, expected.y, 1e-4F + (expected.y * 1e-4F));
        CY_CHECK_NEAR(sampled.z, expected.z, 1e-4F + (expected.z * 1e-4F));
    }

    // In the sRGB encoding a creative LUT is exported in, an inversion of the ENCODED value: red
    // becomes cyan, and sRGB mid-grey (code 128, display-linear 0.2158) maps to itself.
    CubeLut inversion(allocator());
    CY_REQUIRE(parse_cube_lut(cube_text(17,
                                        [](Vec3 in) noexcept {
                                            return Vec3{1.0F - in.x, 1.0F - in.y, 1.0F - in.z};
                                        }),
                              inversion)
                   .has_value());
    grade.cube = &inversion;
    grade.cube_encoding = CubeEncoding::Srgb;
    const Vec3 cyan = apply_display_grade(Vec3{1.0F, 0.0F, 0.0F}, grade);
    CY_CHECK_NEAR(cyan.x, 0.0F, 1e-6F);
    CY_CHECK_NEAR(cyan.y, 1.0F, 1e-5F);
    CY_CHECK_NEAR(cyan.z, 1.0F, 1e-5F);
    const f32 grey = cy::rendering::srgb_decode(128.0F / 255.0F);
    const Vec3 same = apply_display_grade(Vec3{grey, grey, grey}, grade);
    CY_CHECK_NEAR(same.x, cy::rendering::srgb_decode(127.0F / 255.0F), 1e-5F);
}

CY_TEST_CASE("an identity LUT and neutral parameters bake to the identity, and nothing else does") {
    CY_CHECK(display_lut_is_identity(bake(DisplayGrade{}).data(), kDisplayLutSize));

    CubeLut identity(allocator());
    CY_REQUIRE(
        parse_cube_lut(cube_text(17, [](Vec3 in) noexcept { return in; }), identity).has_value());
    DisplayGrade through_cube;
    through_cube.cube = &identity;
    CY_CHECK(display_lut_is_identity(bake(through_cube).data(), kDisplayLutSize));
    through_cube.cube_encoding = CubeEncoding::Linear;
    CY_CHECK(display_lut_is_identity(bake(through_cube).data(), kDisplayLutSize));

    // THE CONTROLS: a quarter-stop of gain on one channel, and a rotation.
    DisplayGrade warm;
    warm.settings.gain = Vec3{1.19F, 1.0F, 1.0F};
    CY_CHECK_FALSE(display_lut_is_identity(bake(warm).data(), kDisplayLutSize));
    CubeLut rotation(allocator());
    CY_REQUIRE(parse_cube_lut(cube_text(2, rotate_channels), rotation).has_value());
    DisplayGrade rotated;
    rotated.cube = &rotation;
    CY_CHECK_FALSE(display_lut_is_identity(bake(rotated).data(), kDisplayLutSize));
}
