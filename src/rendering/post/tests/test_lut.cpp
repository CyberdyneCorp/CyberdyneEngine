// SPDX-License-Identifier: MIT
// The display-referred grading table: `.cube` files, the encoding the runtime indexes in, the bake,
// and `.cygrade` looks. What the graded resolve samples on the device is built here, so every
// property the device suite relies on is first pinned on the host.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/post/exposure.h>
#include <cy/rendering/post/look.h>
#include <cy/rendering/post/lut.h>

#include "cube_fixtures.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec3;
using cy::post_test::cube_text;
using cy::post_test::rotate_channels;
using cy::rendering::apply_display_grade;
using cy::rendering::CubeEncoding;
using cy::rendering::CubeLut;
using cy::rendering::display_log_decode;
using cy::rendering::display_log_encode;
using cy::rendering::DisplayGrade;
using cy::rendering::parse_cube_lut;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

}  // namespace

CY_TEST_CASE("a .cube file parses in its own order, and a malformed one is refused") {
    CubeLut cube(allocator());
    const std::string text = cube_text(
        2, rotate_channels, "DOMAIN_MIN 0 0 0   # the default, stated\nDOMAIN_MAX 1 1 1\n\n");
    CY_REQUIRE(parse_cube_lut(text, cube).has_value());
    CY_CHECK_EQ(cube.size, 2U);
    CY_REQUIRE_EQ(cube.entries.size(), usize{8});
    // RED FASTEST: entry 1 is the lattice point (1, 0, 0), whose rotation is (0, 0, 1).
    CY_CHECK_EQ(cube.entries[1].z, 1.0F);
    CY_CHECK_EQ(cube.entries[1].x, 0.0F);
    // Entry 4 is (0, 0, 1), whose rotation is (0, 1, 0).
    CY_CHECK_EQ(cube.entries[4].y, 1.0F);
    const Vec3 middle = cube.sample(Vec3{0.2F, 0.5F, 0.8F});
    CY_CHECK_NEAR(middle.x, 0.5F, 1e-6F);
    CY_CHECK_NEAR(middle.y, 0.8F, 1e-6F);
    CY_CHECK_NEAR(middle.z, 0.2F, 1e-6F);

    const char* refused[] = {
        "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n",
        "0 0 0\n",
        "LUT_3D_SIZE 2\n0 0 0\n1 1 1\n",
        "LUT_3D_SIZE 1\n0 0 0\n",
        "LUT_3D_SIZE 2\n0 0 0\n0 0 x\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n",
        "LUT_3D_SIZE 2\nGAMMA 2.2\n",
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): one literal, wrapped.
        "LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n"
        "0 0 0\n0 0 0\n0 0 0\n",
    };
    for (const char* bad : refused) {
        CubeLut rejected(allocator());
        const cy::Status parsed = parse_cube_lut(bad, rejected);
        CY_CHECK_FALSE(parsed.has_value());
    }
}

CY_TEST_CASE("the display encoding is exact at black and white and invertible between") {
    CY_CHECK_EQ(display_log_encode(0.0F), 0.0F);
    CY_CHECK_NEAR(display_log_encode(1.0F), 1.0F, 1e-6F);
    CY_CHECK_EQ(display_log_decode(0.0F), 0.0F);
    CY_CHECK_NEAR(display_log_decode(1.0F), 1.0F, 1e-6F);
    for (const f32 value : {1e-5F, 2.4e-4F, 0.001F, 0.01F, 0.18F, 0.5F, 0.9F}) {
        CY_CHECK_NEAR(display_log_decode(display_log_encode(value)), value, value * 1e-5F);
    }
    // THE SHADOWS GET THE TABLE: display-linear 0.01 — sRGB code 25 of 255 — is already a third of
    // the way along the axis, where a linear index would have spent a hundredth.
    CY_CHECK_GT(display_log_encode(0.01F), 0.4F);
    // Twelve stops above the toe, evenly: a stop is 1/12 of the axis there.
    const f32 stop = display_log_encode(0.5F) - display_log_encode(0.25F);
    CY_CHECK_NEAR(stop, 1.0F / 12.0F, 2e-3F);
}

CY_TEST_CASE("the parametric grade runs first and the cube's look after it") {
    CubeLut rotation(allocator());
    CY_REQUIRE(parse_cube_lut(cube_text(2, rotate_channels), rotation).has_value());
    DisplayGrade grade;
    grade.settings.gain = Vec3{0.5F, 1.0F, 1.0F};
    grade.cube = &rotation;
    grade.cube_encoding = CubeEncoding::Linear;
    // Red halved THEN rotated into blue. The other order would halve what arrived in red — green.
    const Vec3 out = apply_display_grade(Vec3{0.8F, 0.4F, 0.2F}, grade);
    CY_CHECK_NEAR(out.x, 0.4F, 1e-5F);
    CY_CHECK_NEAR(out.y, 0.2F, 1e-5F);
    CY_CHECK_NEAR(out.z, 0.4F, 1e-5F);
}

CY_TEST_CASE("a .cygrade look parses every control and refuses an unknown one") {
    const char* text =
        "# a warm look\n"
        "cygrade 1\n"
        "name Warm evening\n"
        "temperature 5200\n"
        "tint 0.02\n"
        "lift 0.01 0 -0.01\n"
        "gamma 1 1.05 1\n"
        "gain 1.06 1 0.92   # a comment after a value\n"
        "contrast 1.08\n"
        "saturation 1.1\n"
        "hue-shift 90\n"
        "shadows-offset 0 0 0.01\n"
        "highlight-boundary 0.7\n"
        "mixer-red 0.9 0.1 0\n"
        "cube looks/teal.cube\n"
        "cube-encoding linear\n"
        "lut-size 17\n";
    cy::rendering::Look look;
    CY_REQUIRE(cy::rendering::parse_look(text, look).has_value());
    CY_CHECK_EQ(std::string(look.name), std::string("Warm evening"));
    CY_CHECK_EQ(look.settings.temperature, 5200.0F);
    CY_CHECK_EQ(look.settings.lift.z, -0.01F);
    CY_CHECK_EQ(look.settings.gain.z, 0.92F);
    CY_CHECK_NEAR(look.settings.hue_shift, 1.5707963F, 1e-6F);
    CY_CHECK_EQ(look.settings.shadows.offset.z, 0.01F);
    CY_CHECK_EQ(look.settings.highlight_boundary, 0.7F);
    CY_CHECK_EQ(look.settings.channel_mixer[0].y, 0.1F);
    CY_CHECK_EQ(std::string(look.cube), std::string("looks/teal.cube"));
    CY_CHECK(look.cube_encoding == CubeEncoding::Linear);
    CY_CHECK_EQ(look.lut_size, 17U);
    CY_CHECK_FALSE(look.settings.neutral());

    const char* refused[] = {
        "temperature 5200\n",                // no header
        "cygrade 2\n",                       // a version this parser does not read
        "cygrade 1\nsaturaton 1.2\n",        // misspelt: refused, not skipped
        "cygrade 1\nlift 0.1 0.1\n",         // two numbers for three channels
        "cygrade 1\ncontrast high\n",        // not a number
        "cygrade 1\ncube-encoding gamma\n",  // not an encoding
        "cygrade 1\nlut-size 1\n",           // too small to interpolate
    };
    for (const char* bad : refused) {
        cy::rendering::Look rejected;
        CY_CHECK_FALSE(cy::rendering::parse_look(bad, rejected).has_value());
    }
    cy::rendering::Look neutral;
    CY_REQUIRE(cy::rendering::parse_look("cygrade 1\n", neutral).has_value());
    CY_CHECK(neutral.settings.neutral());
}

CY_TEST_CASE("a manual EV100 reaches the frame's stops through the saturation constant") {
    using cy::rendering::exposure_stops_for_ev100;
    using cy::rendering::multiplier_for_ev100;
    for (const f32 ev : {-2.0F, 0.0F, 9.5F, 14.0F}) {
        CY_CHECK_NEAR(std::exp2(exposure_stops_for_ev100(ev)), multiplier_for_ev100(ev),
                      multiplier_for_ev100(ev) * 1e-5F);
        CY_CHECK_NEAR(cy::rendering::ev100_for_exposure_stops(exposure_stops_for_ev100(ev)), ev,
                      1e-5F);
        // ONE EV MORE IS HALF THE LIGHT: the multiply the resolve applies halves.
        CY_CHECK_NEAR(multiplier_for_ev100(ev + 1.0F) / multiplier_for_ev100(ev), 0.5F, 1e-6F);
    }
    // Sunny 16: f/16, 1/100 s, ISO 100 is EV100 14.6, and the multiplier puts a surface of the
    // luminance that EV meters at 1/(1.2 * 8) — below the curve's shoulder, as a meter intends.
    cy::rendering::CameraControls sunny;
    sunny.aperture = 16.0F;
    const f32 ev = cy::rendering::ev100_from_camera(sunny);
    CY_CHECK_NEAR(ev, 14.64F, 0.01F);
    CY_CHECK_NEAR(cy::rendering::luminance_for_ev100(ev) * multiplier_for_ev100(ev),
                  1.0F / (1.2F * 8.0F), 1e-5F);
}

CY_TEST_CASE("the histogram's binning and the adaptation's deadline are the metering's own") {
    using cy::rendering::luminance_histogram_bin;
    // A luminance exactly at a bin's centre lands in that bin; below the range is bin 0 and above
    // it the last, so every pixel is counted.
    const f32 width = 20.0F / 256.0F;
    for (const u32 bin : {0U, 17U, 128U, 255U}) {
        const f32 ev = -4.0F + (width * (static_cast<f32>(bin) + 0.5F));
        CY_CHECK_EQ(
            luminance_histogram_bin(cy::rendering::luminance_for_ev100(ev), 256, -4.0F, 16.0F),
            bin);
    }
    CY_CHECK_EQ(luminance_histogram_bin(0.0F, 256, -4.0F, 16.0F), 0U);
    CY_CHECK_EQ(luminance_histogram_bin(1e9F, 256, -4.0F, 16.0F), 255U);

    // THE STATED TIME: `adaptation_seconds(speed, 0.05)` of frames closes 95 % of a step and not
    // much sooner. Stepped at 60 Hz from four stops below the target.
    cy::rendering::AutoExposureSettings settings;
    settings.speed_brightening = 2.0F;
    const f32 deadline = cy::rendering::adaptation_seconds(settings.speed_brightening, 0.05F);
    CY_CHECK_NEAR(deadline, 1.4979F, 1e-3F);
    const f32 dt = 1.0F / 60.0F;
    f32 current = 6.0F;
    f32 elapsed = 0.0F;
    f32 at_ninety = 0.0F;
    while (elapsed < deadline) {
        current = cy::rendering::adapt_ev100(current, 10.0F, dt, settings);
        elapsed += dt;
        if (at_ninety == 0.0F && 10.0F - current <= 0.4F) {
            at_ninety = elapsed;
        }
    }
    CY_CHECK_LE(10.0F - current, (0.05F * 4.0F) + 1e-4F);
    CY_CHECK_NEAR(at_ninety, cy::rendering::adaptation_seconds(2.0F, 0.1F), dt);
}
