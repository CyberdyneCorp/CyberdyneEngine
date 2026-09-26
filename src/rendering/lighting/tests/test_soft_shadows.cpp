// Percentage-closer soft shadows for a directional light, on the host. `render_soft_shadows`.
//
// `cy/shadow.slang`'s three steps, transcribed in `soft_shadows.cpp`, over an analytic shadow map:
// a floor, and a half-plane blocker hanging a known height above it with its edge across the middle
// of the map. The light points straight down the map's depth axis, so the penumbra the blocker
// casts is a band across the edge whose width similar triangles predict exactly — which is what
// makes "the penumbra widens with blocker distance" a measurement rather than a picture to look at.
//
// THE PROFILE IS AN AVERAGE OVER ROWS. The disc is rotated per pixel by interleaved gradient noise,
// so one row's profile is noisy by design; averaging 48 rows is what a small filter or TAA does to
// the same noise, and gives the edge its expected shape.

#include <cy/rendering/lighting/soft_shadows.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::usize;
using cy::rendering::DirectionalShadowFootprint;
using cy::rendering::PcssShape;
using cy::rendering::ShadowDepthMap;
using cy::rendering::SoftShadowSettings;

constexpr u32 kExtent = 1024;
constexpr f32 kWidthMetres = 4.0F;
constexpr f32 kDepthRangeMetres = 10.0F;
/// The floor's stored depth. Reversed-Z: nearer the light is greater.
constexpr f32 kFloor = 0.2F;
/// `cy/frame.slang`'s constant receiver bias.
constexpr f32 kReceiverBias = 0.0005F;
constexpr u32 kRows = 48;

/// A floor, and — when `height` is positive — a blocker `height` metres above it covering u < 0.5.
std::vector<f32> half_plane_map(f32 height) {
    std::vector<f32> depths(static_cast<usize>(kExtent) * kExtent, kFloor);
    if (height <= 0.0F) {
        return depths;
    }
    const f32 blocker = kFloor + (height / kDepthRangeMetres);
    for (u32 y = 0; y < kExtent; ++y) {
        for (u32 x = 0; x < kExtent / 2U; ++x) {
            depths[(static_cast<usize>(y) * kExtent) + x] = blocker;
        }
    }
    return depths;
}

PcssShape shape_for(f32 angular_radius) {
    SoftShadowSettings settings;
    settings.angular_radius = angular_radius;
    settings.max_radius_texels = 64.0F;
    DirectionalShadowFootprint footprint;
    footprint.depth_range_metres = kDepthRangeMetres;
    footprint.width_metres = kWidthMetres;
    footprint.extent = kExtent;
    return cy::rendering::make_pcss_shape(settings, footprint);
}

/// The floor's visibility at `u`, averaged over `kRows` rows of per-pixel rotation.
f32 averaged_visibility(const ShadowDepthMap& map, const PcssShape& shape, f32 u) {
    f32 total = 0.0F;
    const f32 pixel_x = std::floor(u * static_cast<f32>(kExtent)) + 0.5F;
    for (u32 row = 0; row < kRows; ++row) {
        const f32 v = (static_cast<f32>(row) + 0.5F) / static_cast<f32>(kRows);
        const f32 rotation =
            cy::rendering::shadow_disc_rotation(pixel_x, static_cast<f32>(row) + 0.5F);
        total += cy::rendering::pcss_visibility_reference(map, u, v, kFloor + kReceiverBias, shape,
                                                          rotation);
    }
    return total / static_cast<f32>(kRows);
}

/// The width, in texels, over which the floor goes from 10 % to 90 % lit across the blocker's
/// edge: the penumbra.
f32 penumbra_texels(f32 height, f32 angular_radius) {
    const std::vector<f32> depths = half_plane_map(height);
    const ShadowDepthMap map{cy::Span<const f32>(depths.data(), depths.size()), kExtent};
    const PcssShape shape = shape_for(angular_radius);
    f32 dark_end = -1.0F;
    f32 lit_start = -1.0F;
    // One sample a texel, 160 texels either side of the edge: wider than any kernel these cases
    // ask for.
    for (u32 texel = (kExtent / 2U) - 160U; texel < (kExtent / 2U) + 160U; ++texel) {
        const f32 u = (static_cast<f32>(texel) + 0.5F) / static_cast<f32>(kExtent);
        const f32 visibility = averaged_visibility(map, shape, u);
        if (dark_end < 0.0F && visibility > 0.1F) {
            dark_end = static_cast<f32>(texel);
        }
        if (lit_start < 0.0F && visibility >= 0.9F) {
            lit_start = static_cast<f32>(texel);
            break;
        }
    }
    CY_REQUIRE(dark_end >= 0.0F);
    CY_REQUIRE(lit_start >= 0.0F);
    return lit_start - dark_end;
}

}  // namespace

CY_TEST_CASE("a penumbra widens with the distance between the blocker and the receiver") {
    // Similar triangles: the penumbra's radius is `height * tan(angular radius)`, so doubling the
    // height doubles the band. 0.05 rad is a large source (a 5.7-degree disc) chosen so the
    // smallest band here is several texels wide and the ratio is measured rather than quantised.
    constexpr f32 kRadius = 0.05F;
    const f32 low = penumbra_texels(0.25F, kRadius);
    const f32 middle = penumbra_texels(0.5F, kRadius);
    const f32 high = penumbra_texels(1.0F, kRadius);
    std::fprintf(stderr, "penumbra (10-90%%): %.1f, %.1f, %.1f texels at 0.25, 0.5, 1 m\n",
                 static_cast<double>(low), static_cast<double>(middle), static_cast<double>(high));
    CY_CHECK_GT(middle, low);
    CY_CHECK_GT(high, middle);
    // Proportional, not merely increasing: each doubling of the height roughly doubles the band.
    CY_CHECK_GT(middle / low, 1.6F);
    CY_CHECK_LT(middle / low, 2.5F);
    CY_CHECK_GT(high / middle, 1.6F);
    CY_CHECK_LT(high / middle, 2.5F);
}

CY_TEST_CASE("softness follows the light's angular radius, with nothing else to tune") {
    const f32 narrow = penumbra_texels(0.5F, 0.025F);
    const f32 wide = penumbra_texels(0.5F, 0.05F);
    std::fprintf(stderr, "penumbra at 0.5 m: %.1f texels for 0.025 rad, %.1f for 0.05 rad\n",
                 static_cast<double>(narrow), static_cast<double>(wide));
    CY_CHECK_GT(wide / narrow, 1.6F);
    CY_CHECK_LT(wide / narrow, 2.5F);
}

CY_TEST_CASE("a blocker touching its receiver casts the minimum kernel: contact hardens") {
    // Two centimetres: the penumbra radius is a tenth of a texel, so the kernel is clamped to the
    // minimum of one texel and the edge is as sharp as the old 3x3 filter's.
    const f32 contact = penumbra_texels(0.02F, 0.05F);
    const f32 distant = penumbra_texels(1.0F, 0.05F);
    std::fprintf(stderr, "penumbra: %.1f texels at contact, %.1f at 1 m\n",
                 static_cast<double>(contact), static_cast<double>(distant));
    CY_CHECK_LE(contact, 2.0F);
    CY_CHECK_GT(distant, 8.0F * contact);
}

CY_TEST_CASE("an open plane is lit exactly and an umbra is dark exactly: no leak, no noise") {
    const PcssShape shape = shape_for(0.05F);
    // THE OPEN PLANE. No blocker anywhere, so the search finds none and every receiver is exactly
    // lit — not lit on average: a filter that leaked a little shadow onto a bare floor would
    // speckle every open surface in the frame.
    const std::vector<f32> open = half_plane_map(0.0F);
    const ShadowDepthMap open_map{cy::Span<const f32>(open.data(), open.size()), kExtent};
    u32 not_lit = 0;
    for (u32 y = 0; y < kExtent; y += 7U) {
        for (u32 x = 0; x < kExtent; x += 7U) {
            const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(kExtent);
            const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(kExtent);
            const f32 rotation = cy::rendering::shadow_disc_rotation(static_cast<f32>(x) + 0.5F,
                                                                     static_cast<f32>(y) + 0.5F);
            not_lit += cy::rendering::pcss_visibility_reference(
                           open_map, u, v, kFloor + kReceiverBias, shape, rotation) != 1.0F
                           ? 1U
                           : 0U;
        }
    }
    CY_CHECK_EQ(not_lit, 0U);

    // THE UMBRA. Deep under the blocker — farther from its edge than the widest kernel — every tap
    // is blocked, so no light leaks in however the disc is rotated.
    const std::vector<f32> covered = half_plane_map(1.0F);
    const ShadowDepthMap covered_map{cy::Span<const f32>(covered.data(), covered.size()), kExtent};
    u32 not_dark = 0;
    for (u32 y = 0; y < kExtent; y += 5U) {
        for (u32 x = 16; x < (kExtent / 2U) - 80U; x += 5U) {
            const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(kExtent);
            const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(kExtent);
            const f32 rotation = cy::rendering::shadow_disc_rotation(static_cast<f32>(x) + 0.5F,
                                                                     static_cast<f32>(y) + 0.5F);
            not_dark += cy::rendering::pcss_visibility_reference(
                            covered_map, u, v, kFloor + kReceiverBias, shape, rotation) != 0.0F
                            ? 1U
                            : 0U;
        }
    }
    CY_CHECK_EQ(not_dark, 0U);
}

CY_TEST_CASE("the shape is the light's and the map's, and the tap counts are the quality lever's") {
    SoftShadowSettings settings;
    DirectionalShadowFootprint footprint;
    footprint.depth_range_metres = 40.0F;
    footprint.width_metres = 20.0F;
    footprint.extent = 2048;
    const PcssShape shape = cy::rendering::make_pcss_shape(settings, footprint);
    CY_CHECK_NEAR(shape.penumbra_per_depth,
                  40.0F * std::tan(cy::rendering::kSunAngularRadius) / 20.0F, 1e-7F);
    CY_CHECK_NEAR(shape.min_radius, 1.0F / 2048.0F, 1e-9F);
    CY_CHECK_NEAR(shape.max_radius, 24.0F / 2048.0F, 1e-9F);
    CY_CHECK_EQ(shape.filter_taps,
                cy::rendering::shadow_sample_count(cy::rendering::ShadowQuality::Ultra));

    // The quality tier moves the taps and nothing else.
    settings.quality = cy::rendering::ShadowQuality::Medium;
    const PcssShape cheaper = cy::rendering::make_pcss_shape(settings, footprint);
    CY_CHECK_LT(cheaper.filter_taps, shape.filter_taps);
    CY_CHECK_EQ(cheaper.penumbra_per_depth, shape.penumbra_per_depth);

    // A map with no footprint gives a zero kernel — a hard shadow, never a garbage one.
    const PcssShape empty = cy::rendering::make_pcss_shape(settings, DirectionalShadowFootprint{});
    CY_CHECK_EQ(empty.penumbra_per_depth, 0.0F);
    CY_CHECK_EQ(empty.max_radius, 0.0F);

    cy::u32 control[4] = {};
    f32 words[4] = {};
    cy::rendering::write_soft_shadow_words(3U, 7U, shape, control, words);
    CY_CHECK_EQ(control[0], 3U);
    CY_CHECK_EQ(control[1], 7U);
    CY_CHECK_EQ(control[2], shape.blocker_taps);
    CY_CHECK_EQ(control[3], shape.filter_taps);
    CY_CHECK_EQ(words[0], shape.penumbra_per_depth);
    CY_CHECK_EQ(words[1], shape.min_radius);
    CY_CHECK_EQ(words[2], shape.max_radius);
}
