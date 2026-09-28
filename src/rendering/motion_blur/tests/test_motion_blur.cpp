// SPDX-License-Identifier: MIT
// Motion blur on the host: the shutter arithmetic, the constants, and the reference filter over
// synthetic frames whose every pixel's colour, depth and motion the case wrote itself.
// `integration.rendering_motion_blur`.
//
// The frames are one row of pixels repeated down a short strip: a BAR at 5 m in front of a
// BACKGROUND at 10 m, one of the two moving horizontally. That is the requirement's two scenarios
// reduced to the only dimension they are about — how far along the motion a pixel's colour came
// from — and it is small enough that every pixel of the reference can be checked.

#include "motion_measure.h"

#include <cy/core/math/projection.h>
#include <cy/core/math/quat.h>
#include <cy/rendering/motion_blur/motion_blur.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace cy;
using namespace cy::rendering::motion_blur;

namespace {

constexpr u32 kWidth = 192;
constexpr u32 kHeight = 24;
constexpr usize kPixels = static_cast<usize>(kWidth) * kHeight;
/// The bar spans these columns.
constexpr u32 kBarLeft = 80;
constexpr u32 kBarRight = 120;
constexpr f32 kBarDepth = 5.0F;
constexpr f32 kBackgroundDepth = 10.0F;
/// Pixels the moving thing travels in a frame.
constexpr f32 kMotion = 24.0F;

[[nodiscard]] Mat4 projection() noexcept {
    return perspective_reversed_z(0.9F, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F,
                                  100.0F);
}

/// The reversed-Z depth sample a view depth is written as: the inverse of the filter's
/// `m32 / (d + m22)`.
[[nodiscard]] f32 device_depth(f32 metres) noexcept {
    const Mat4 p = projection();
    return (p.columns[3].z / metres) - p.columns[2].z;
}

enum class Moving : u8 { Bar, Background };

/// One synthetic frame.
struct Frame {
    std::vector<Vec4> color = std::vector<Vec4>(kPixels);
    std::vector<Vec2> velocity = std::vector<Vec2>(kPixels, Vec2{0.0F, 0.0F});
    std::vector<f32> depth = std::vector<f32>(kPixels);

    [[nodiscard]] MotionBlurInputs inputs() const noexcept {
        MotionBlurInputs in;
        in.width = kWidth;
        in.height = kHeight;
        in.color = Span<const Vec4>(color.data(), color.size());
        in.velocity = Span<const Vec2>(velocity.data(), velocity.size());
        in.depth = Span<const f32>(depth.data(), depth.size());
        return in;
    }
};

[[nodiscard]] bool on_bar(u32 x) noexcept {
    return x >= kBarLeft && x < kBarRight;
}

/// The bar in front of the background, one of them moving `kMotion` pixels right since last frame —
/// so its motion vector, which points at the past, is `-kMotion` pixels. `striped` gives the
/// background two-pixel stripes, which is what a smear of it would show.
[[nodiscard]] Frame make_frame(Moving moving, bool striped) noexcept {
    Frame frame;
    const Vec2 past{-kMotion / static_cast<f32>(kWidth), 0.0F};
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            const bool bar = on_bar(x);
            const f32 stripe = striped && ((x / 2U) % 2U) == 0U ? 1.0F : 0.25F;
            frame.color[at] =
                bar ? Vec4{8.0F, 6.0F, 2.0F, 1.0F} : Vec4{stripe, stripe, stripe, 1.0F};
            frame.depth[at] = device_depth(bar ? kBarDepth : kBackgroundDepth);
            const bool moves = bar ? moving == Moving::Bar : moving == Moving::Background;
            frame.velocity[at] = moves ? past : Vec2{0.0F, 0.0F};
        }
    }
    return frame;
}

[[nodiscard]] MotionBlurView still_view() noexcept {
    MotionBlurView view;
    view.width = kWidth;
    view.height = kHeight;
    view.projection = projection();
    view.relative_to_clip = projection();
    view.previous_relative_to_clip = projection();
    return view;
}

[[nodiscard]] MotionBlurConstants constants_at(f32 shutter_degrees) noexcept {
    MotionBlurSettings settings;
    settings.shutter_angle_degrees = shutter_degrees;
    Expected<MotionBlurConstants, Error> made = make_motion_blur_constants(settings, still_view());
    CY_REQUIRE(made.has_value());
    return *made;
}

[[nodiscard]] std::vector<Vec4> blurred(const Frame& frame, const MotionBlurConstants& constants) {
    std::vector<Vec4> out(kPixels);
    CY_REQUIRE(motion_blur_reference(frame.inputs(), constants, Span<Vec4>(out.data(), out.size()))
                   .has_value());
    return out;
}

/// The middle row's luminance.
[[nodiscard]] std::vector<f32> middle_row(const std::vector<Vec4>& image) {
    std::vector<f32> row(kWidth);
    const usize base = static_cast<usize>(kHeight / 2U) * kWidth;
    for (u32 x = 0; x < kWidth; ++x) {
        const Vec4& texel = image[base + x];
        row[x] = motion_test::luminance(texel.x, texel.y, texel.z);
    }
    return row;
}

[[nodiscard]] bool same_bits(const Vec4& a, const Vec4& b) noexcept {
    return std::memcmp(&a, &b, sizeof(Vec4)) == 0;
}

}  // namespace

CY_TEST_CASE("the shutter angle is the fraction of the frame the shutter is open") {
    CY_CHECK_NEAR(shutter_angle_for(1.0F / 120.0F, 1.0F / 60.0F), 180.0F, 1e-3F);
    // `CameraControls`' default shutter at sixty frames a second.
    CY_CHECK_NEAR(shutter_angle_for(1.0F / 100.0F, 1.0F / 60.0F), 216.0F, 1e-3F);
    // A shutter cannot stay open longer than the frame it exposes, nor for no frame at all.
    CY_CHECK_EQ(shutter_angle_for(1.0F / 30.0F, 1.0F / 60.0F), 360.0F);
    CY_CHECK_EQ(shutter_angle_for(1.0F / 100.0F, 0.0F), 0.0F);
    CY_CHECK_EQ(shutter_angle_for(0.0F, 1.0F / 60.0F), 0.0F);
    const MotionBlurSettings camera = settings_for_camera({}, 1.0F / 120.0F, 1.0F / 60.0F);
    CY_CHECK_NEAR(camera.shutter_angle_degrees, 180.0F, 1e-3F);
}

CY_TEST_CASE("the constants carry the shutter as a radius, and refuse what cannot blur") {
    // 180 degrees: half a frame of motion, centred on the frame, so a quarter to each side.
    const MotionBlurConstants half = constants_at(180.0F);
    CY_CHECK_NEAR(half.scales[0], 0.25F, 1e-6F);
    CY_CHECK_NEAR(half.scales[1], 0.25F, 1e-6F);
    CY_CHECK_EQ(half.depth[2], 0.0F);
    CY_CHECK_EQ(half.control[1], 24U);
    CY_CHECK_EQ(half.control[2], 8U);
    CY_CHECK_EQ(half.control[3], 1U);
    CY_CHECK(blurs(half));
    CY_CHECK_FALSE(blurs(constants_at(0.0F)));

    MotionBlurSettings settings;
    settings.camera_scale = 0.0F;
    Expected<MotionBlurConstants, Error> split = make_motion_blur_constants(settings, still_view());
    CY_REQUIRE(split.has_value());
    CY_CHECK_EQ(split->depth[2], 1.0F);

    MotionBlurSettings even;
    even.samples = 14;
    CY_CHECK_FALSE(make_motion_blur_constants(even, still_view()).has_value());
    MotionBlurSettings flat;
    flat.max_radius_pixels = 0;
    CY_CHECK_FALSE(make_motion_blur_constants(flat, still_view()).has_value());
    MotionBlurSettings backwards;
    backwards.object_scale = -1.0F;
    CY_CHECK_FALSE(make_motion_blur_constants(backwards, still_view()).has_value());
    MotionBlurView empty = still_view();
    empty.width = 0;
    CY_CHECK_FALSE(make_motion_blur_constants({}, empty).has_value());
}

CY_TEST_CASE("a closed shutter copies every pixel, bit for bit") {
    const Frame frame = make_frame(Moving::Bar, true);
    const std::vector<Vec4> out = blurred(frame, constants_at(0.0F));
    for (usize at = 0; at < kPixels; ++at) {
        CY_REQUIRE(same_bits(out[at], frame.color[at]));
    }
}

CY_TEST_CASE("the streak is the shutter's fraction of the motion") {
    const Frame frame = make_frame(Moving::Bar, false);
    const std::vector<f32> unblurred = middle_row(frame.color);
    f32 reaches[3] = {};
    const f32 angles[3] = {90.0F, 180.0F, 360.0F};
    for (u32 index = 0; index < 3U; ++index) {
        const std::vector<f32> row = middle_row(blurred(frame, constants_at(angles[index])));
        // How far past the bar's right edge the blur reaches — half the shutter-open motion for a
        // physical shutter. `motion_measure.h` says why this and not the edge's ramp.
        const f32 radius = kMotion * angles[index] / 720.0F;
        reaches[index] = motion_test::smear_reach(unblurred, row, kBarRight, 16,
                                                  unblurred[kBarRight - 1U], 0.01F);
        std::fprintf(stderr, "shutter %.0f: the blur reaches %.1f px past the edge, of %.1f\n",
                     static_cast<double>(angles[index]), static_cast<double>(reaches[index]),
                     static_cast<double>(radius));
        // Never farther than the shutter lets it — within the texel the tip lands in — and short
        // of it only by the cone's last percent. Measured 2, 5 and 10 of 3, 6 and 12.
        CY_CHECK_LE(reaches[index], radius + 0.5F);
        CY_CHECK_GE(reaches[index], 0.6F * radius);
    }
    // 180 degrees is half a frame of motion: the requirement's own scenario, as a ratio. Whole
    // texels, so 5 against 2 at the shortest.
    CY_CHECK_GE(reaches[2] / reaches[1], 1.7F);
    CY_CHECK_LE(reaches[2] / reaches[1], 2.6F);
    CY_CHECK_GE(reaches[1] / reaches[0], 1.7F);
    CY_CHECK_LE(reaches[1] / reaches[0], 2.6F);
}

CY_TEST_CASE("a fast bar blurs and the background outside its streak stays sharp") {
    const Frame frame = make_frame(Moving::Bar, true);
    const MotionBlurConstants constants = constants_at(360.0F);
    const std::vector<Vec4> out = blurred(frame, constants);
    // The streak reaches half the motion to each side of the bar; the background beyond it — even
    // inside the tiles the bar's motion reaches — keeps every stripe.
    const u32 reach = static_cast<u32>(std::ceil(kMotion * 0.5F)) + 1U;
    f32 worst_background = 0.0F;
    f32 bar_change = 0.0F;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            const f32 change = std::fabs(out[at].x - frame.color[at].x);
            if (x + reach < kBarLeft || x >= kBarRight + reach) {
                worst_background = std::fmax(worst_background, change / frame.color[at].x);
            } else if (x == kBarRight - 2U) {
                bar_change = std::fmax(bar_change, change);
            }
        }
    }
    std::fprintf(stderr, "background outside the streak: worst relative change %g\n",
                 static_cast<double>(worst_background));
    CY_CHECK_LT(worst_background, 1e-5F);
    // The control: the bar's own edge is a mixture of bar and background.
    // Measured 0.98 of the 7.75 between them two texels in: McGuire's cone mixes the background
    // into a moving foreground's edge less than a shutter would, and the case only needs it to be
    // a mixture at all.
    CY_CHECK_GT(bar_change, 0.5F);
}

CY_TEST_CASE("a still foreground over a moving background does not take the background's smear") {
    const Frame frame = make_frame(Moving::Background, true);
    const std::vector<Vec4> out = blurred(frame, constants_at(360.0F));
    f32 worst_bar = 0.0F;
    f32 background_change = 0.0F;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            const f32 change = std::fabs(out[at].x - frame.color[at].x) / frame.color[at].x;
            if (on_bar(x)) {
                worst_bar = std::fmax(worst_bar, change);
            } else if (x < kBarLeft - 16U) {
                background_change = std::fmax(background_change, change);
            }
        }
    }
    std::fprintf(stderr, "still bar over a moving background: worst relative change %g\n",
                 static_cast<double>(worst_bar));
    CY_CHECK_LT(worst_bar, 1e-5F);
    // The control: the moving background's stripes are smeared flat.
    CY_CHECK_GT(background_change, 0.2F);
}

CY_TEST_CASE("the camera's share and the objects' are scaled apart") {
    // A still world under a camera that moved: every pixel's motion IS the camera's, written as the
    // prepass would write it, so the objects' share is nothing.
    MotionBlurView view = still_view();
    view.previous_relative_to_clip = projection() * Mat4::from_translation(Vec3{3.0F, 0.0F, 0.0F});
    MotionBlurSettings camera_only;
    camera_only.shutter_angle_degrees = 360.0F;
    camera_only.object_scale = 0.0F;
    Expected<MotionBlurConstants, Error> panning = make_motion_blur_constants(camera_only, view);
    CY_REQUIRE(panning.has_value());
    Frame frame = make_frame(Moving::Background, true);
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            const Vec2 camera = camera_motion_pixels(*panning, x, y, frame.depth[at]);
            frame.velocity[at] =
                Vec2{camera.x / static_cast<f32>(kWidth), camera.y / static_cast<f32>(kHeight)};
        }
    }
    const f32 background_motion = std::fabs(frame.velocity[0].x * static_cast<f32>(kWidth));
    std::fprintf(stderr, "camera motion at the background: %.2f px\n",
                 static_cast<double>(background_motion));
    CY_REQUIRE(background_motion > 4.0F);

    // Objects only: the camera's motion is the whole of it, so nothing blurs.
    MotionBlurSettings objects_only;
    objects_only.shutter_angle_degrees = 360.0F;
    objects_only.camera_scale = 0.0F;
    Expected<MotionBlurConstants, Error> objects = make_motion_blur_constants(objects_only, view);
    CY_REQUIRE(objects.has_value());
    const std::vector<Vec4> still = blurred(frame, *objects);
    f32 worst = 0.0F;
    for (usize at = 0; at < kPixels; ++at) {
        worst = std::fmax(worst, std::fabs(still[at].x - frame.color[at].x) / frame.color[at].x);
    }
    CY_CHECK_LT(worst, 1e-5F);

    // Camera only: the same frame blurs.
    const std::vector<Vec4> panned = blurred(frame, *panning);
    f32 smeared = 0.0F;
    for (usize at = 0; at < kPixels; ++at) {
        smeared =
            std::fmax(smeared, std::fabs(panned[at].x - frame.color[at].x) / frame.color[at].x);
    }
    CY_CHECK_GT(smeared, 0.2F);

    // AND THE OTHER WAY ROUND, under a still camera: a bar moving on its own is the objects'
    // share alone, so objects-only blurs it and camera-only leaves every pixel as it was.
    const Frame moving_bar = make_frame(Moving::Bar, true);
    Expected<MotionBlurConstants, Error> still_objects =
        make_motion_blur_constants(objects_only, still_view());
    Expected<MotionBlurConstants, Error> still_camera =
        make_motion_blur_constants(camera_only, still_view());
    CY_REQUIRE(still_objects.has_value());
    CY_REQUIRE(still_camera.has_value());
    const std::vector<Vec4> object_blur = blurred(moving_bar, *still_objects);
    const std::vector<Vec4> camera_blur = blurred(moving_bar, *still_camera);
    f32 object_change = 0.0F;
    f32 camera_change = 0.0F;
    for (usize at = 0; at < kPixels; ++at) {
        const f32 base = moving_bar.color[at].x;
        object_change = std::fmax(object_change, std::fabs(object_blur[at].x - base) / base);
        camera_change = std::fmax(camera_change, std::fabs(camera_blur[at].x - base) / base);
    }
    CY_CHECK_GT(object_change, 0.2F);
    CY_CHECK_LT(camera_change, 1e-5F);
}

CY_TEST_CASE("the tile maxima are the longest vectors, and their neighbourhoods reach one tile") {
    const Frame frame = make_frame(Moving::Bar, false);
    const MotionBlurConstants constants = constants_at(360.0F);
    const usize tiles = static_cast<usize>(constants.control[2]) * constants.control[3];
    std::vector<Vec2> maxima(tiles);
    std::vector<Vec2> neighbours(tiles);
    CY_REQUIRE(motion_blur_tiles_reference(frame.inputs(), constants,
                                           Span<Vec2>(maxima.data(), maxima.size()),
                                           Span<Vec2>(neighbours.data(), neighbours.size()))
                   .has_value());
    // The bar spans columns 80 to 119: tiles 3 and 4 of eight 24-pixel tiles.
    for (u32 tile = 0; tile < constants.control[2]; ++tile) {
        const bool holds_bar = tile == 3U || tile == 4U;
        CY_CHECK_NEAR(std::fabs(maxima[tile].x), holds_bar ? kMotion * 0.5F : 0.0F, 1e-4F);
        const bool reached = tile >= 2U && tile <= 5U;
        CY_CHECK_NEAR(std::fabs(neighbours[tile].x), reached ? kMotion * 0.5F : 0.0F, 1e-4F);
    }
}

CY_TEST_CASE("a turning camera blurs the sky under an infinite projection to finite colour") {
    // REGRESSION. Under `perspective_reversed_z_infinite` m22 is zero, and the filter's view depth
    // of a sky texel, `m32 / (0 + m22)`, was infinity: two sky texels then ordered as `inf - inf`,
    // every weight between them was NaN, and a turning camera painted its whole sky NaN.
    const Mat4 infinite = perspective_reversed_z_infinite(
        0.9F, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F);
    MotionBlurView view;
    view.width = kWidth;
    view.height = kHeight;
    view.projection = infinite;
    view.relative_to_clip = infinite;
    view.previous_relative_to_clip =
        infinite * Mat4::from_quat(Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, 0.05F));
    MotionBlurSettings settings;
    settings.shutter_angle_degrees = 360.0F;
    Expected<MotionBlurConstants, Error> constants = make_motion_blur_constants(settings, view);
    CY_REQUIRE(constants.has_value());

    Frame frame = make_frame(Moving::Bar, true);
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            if (!on_bar(x)) {
                frame.depth[at] = 0.0F;  // the sky: nothing drawn, the cleared far plane
            }
        }
    }
    const std::vector<Vec4> out = blurred(frame, *constants);
    usize non_finite = 0;
    f32 smeared = 0.0F;
    for (usize at = 0; at < kPixels; ++at) {
        non_finite += static_cast<usize>(!std::isfinite(out[at].x) || !std::isfinite(out[at].y) ||
                                         !std::isfinite(out[at].z));
        if (frame.depth[at] == 0.0F) {
            smeared = std::fmax(smeared, std::fabs(out[at].x - frame.color[at].x));
        }
    }
    CY_CHECK_EQ(non_finite, usize{0});
    // And the sky did blur: the turn is several pixels, across two-pixel stripes.
    CY_CHECK_GT(smeared, 0.1F);
}
