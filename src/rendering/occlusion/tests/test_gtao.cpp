// The horizon search's arithmetic, on the host. `unit.rendering_occlusion`.
//
// `gtao_reference` is gtao.slang transcribed, and `render.ambient_occlusion` compares the device
// against it buffer for buffer. So the properties the requirement is about are asserted HERE, where
// a failure names an expression rather than a driver: an open plane is exactly unoccluded, an inner
// corner is occluded and more so nearer its edge, the bent normal leans out of the corner, and the
// cascade is configured by the shared denoiser's own table.

#include "corner_scene.h"

#include <cy/rendering/denoise/denoiser.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace cy;
using namespace cy::occlusion_test;
using cy::rendering::AmbientOcclusionSettings;
using cy::rendering::occlusion::GtaoSettings;
using cy::rendering::occlusion::make_filter_constants;
using cy::rendering::occlusion::make_gtao_constants;

namespace {

/// The analytic scenes' extent. The reference is sampled rather than walked; see `run_reference`.
constexpr u32 kWidth = 96;
constexpr u32 kHeight = 72;

/// The reference at the pixels a case asks about, and unoccluded elsewhere: the unit budget is a
/// millisecond a case and the host march costs microseconds a pixel, so a case names the pixels it
/// reads rather than paying for the image. `sampled` says which were computed.
struct Sampled {
    std::vector<Vec4> values;
    std::vector<u8> sampled;
};

template <typename Wanted>
Sampled run_reference(const CornerScene& scene, const GtaoSettings& settings, Wanted wanted) {
    const auto constants = make_gtao_constants(settings, scene.view);
    CY_REQUIRE(constants.has_value());
    rendering::occlusion::GtaoInputs inputs;
    inputs.width = scene.view.width;
    inputs.height = scene.view.height;
    inputs.depth = Span<const f32>(scene.depth.data(), scene.depth.size());
    inputs.normals = Span<const Vec2>(scene.normals.data(), scene.normals.size());
    Sampled out;
    out.values.assign(scene.depth.size(), Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    out.sampled.assign(scene.depth.size(), 0);
    for (u32 y = 0; y < inputs.height; ++y) {
        for (u32 x = 0; x < inputs.width; ++x) {
            const usize index = (static_cast<usize>(y) * inputs.width) + x;
            if (!wanted(x, y, index)) {
                continue;
            }
            out.values[index] = rendering::occlusion::gtao_reference_at(inputs, *constants, x, y);
            out.sampled[index] = 1;
        }
    }
    return out;
}

/// Every `stride`-th pixel each way.
auto every(u32 stride) {
    return [stride](u32 x, u32 y, usize /*index*/) { return x % stride == 1 && y % stride == 1; };
}

GtaoSettings corner_settings() {
    GtaoSettings settings;
    settings.shared.radius = 1.0F;
    return settings;
}

}  // namespace

CY_TEST_CASE("an open plane is exactly unoccluded, whatever the slice count") {
    const CornerScene scene = make_corner_scene(false, kWidth, kHeight);
    for (const u32 slices : {1U, 4U, 8U}) {
        GtaoSettings settings = corner_settings();
        settings.slices = slices;
        const Sampled out = run_reference(scene, settings, every(10));
        f32 lowest = 1.0F;
        u32 floor_pixels = 0;
        for (usize pixel = 0; pixel < out.values.size(); ++pixel) {
            if (scene.surface[pixel] == 1 && out.sampled[pixel] != 0) {
                lowest = std::min(lowest, out.values[pixel].w);
                ++floor_pixels;
            }
        }
        CY_REQUIRE((floor_pixels) > (20U));
        // Exact up to the float rounding of the ratio: the plane's own samples never rise above
        // the tangent the integral is normalised against.
        CY_CHECK_GE(lowest, 1.0F - 1.0e-5F);
    }
}

CY_TEST_CASE("an inner corner is occluded, and more so nearer its edge") {
    const CornerScene scene = make_corner_scene(true, kWidth, kHeight);
    // The band along the edge in every fourth column, and the rest of the image sparsely.
    const Sampled sampled =
        run_reference(scene, corner_settings(), [&scene](u32 x, u32 y, usize index) {
            const f32 distance = scene.corner_distance[index];
            return (distance < 0.15F && x % 4 == 0) ||
                   (distance > 1.2F && x % 8 == 1 && y % 8 == 1);
        });
    const std::vector<Vec4>& out = sampled.values;
    f32 near_sum = 0.0F;
    f32 near_count = 0.0F;
    f32 far_lowest = 1.0F;
    f32 far_count = 0.0F;
    for (usize pixel = 0; pixel < out.size(); ++pixel) {
        if (scene.surface[pixel] == 0 || sampled.sampled[pixel] == 0) {
            continue;
        }
        const f32 distance = scene.corner_distance[pixel];
        if (distance < 0.15F) {
            near_sum += out[pixel].w;
            near_count += 1.0F;
        } else if (distance > 1.2F) {
            far_lowest = std::min(far_lowest, out[pixel].w);
            far_count += 1.0F;
        }
    }
    CY_REQUIRE((near_count) > (20.0F));
    CY_REQUIRE((far_count) > (40.0F));
    // A 90-degree corner hides a quarter of each surface's cosine-weighted hemisphere at its edge
    // and none beyond the radius. The band within 15 cm of the edge sits between those.
    const f32 near_mean = near_sum / near_count;
    CY_CHECK_LT(near_mean, 0.85F);
    CY_CHECK_GT(near_mean, 0.4F);
    CY_CHECK_GE(far_lowest, 1.0F - 1.0e-5F);
}

CY_TEST_CASE("the bent normal leans out of the corner") {
    const CornerScene scene = make_corner_scene(true, kWidth, kHeight);
    const Sampled sampled =
        run_reference(scene, corner_settings(), [&scene](u32 x, u32 /*y*/, usize index) {
            return scene.surface[index] == 1 && scene.corner_distance[index] < 0.2F && x % 4 == 0;
        });
    const std::vector<Vec4>& out = sampled.values;
    // A floor pixel near the wall: its unoccluded directions lean away from the wall, toward +Z.
    u32 checked = 0;
    for (usize pixel = 0; pixel < out.size(); ++pixel) {
        if (scene.surface[pixel] != 1 || sampled.sampled[pixel] == 0 ||
            scene.corner_distance[pixel] > 0.2F) {
            continue;
        }
        const Vec3 bent = normalize(Vec3{out[pixel].x, out[pixel].y, out[pixel].z});
        CY_CHECK_GT(bent.z, 0.05F);
        CY_CHECK_GT(bent.y, 0.5F);
        ++checked;
    }
    CY_CHECK_GT(checked, 10U);
}

CY_TEST_CASE("the cascade is configured by the shared denoiser's ambient occlusion table") {
    const auto config =
        rendering::denoise::default_config(rendering::denoise::SignalKind::AmbientOcclusion);
    CY_CHECK(config.domain == rendering::denoise::SignalDomain::Visibility);
    const CornerScene scene = make_corner_scene(true, kWidth, kHeight);
    const auto filter = make_filter_constants(scene.view, 2);
    CY_CHECK_EQ(filter.sigma[0], config.sigma_depth);
    CY_CHECK_EQ(filter.sigma[1], config.sigma_normal);
    CY_CHECK_EQ(filter.sigma[2], config.sigma_value);
    CY_CHECK_EQ(filter.control[0], 2U);
    CY_CHECK_EQ(filter.control[1], rendering::denoise::quality_ladder()[0].kernel_extent);
    CY_CHECK_EQ(rendering::occlusion::filter_pass_count(),
                std::min(config.max_passes, rendering::denoise::quality_ladder()[0].max_passes));
}

CY_TEST_CASE("the frame's occlusion words leave direct light alone unless asked") {
    u32 words[4] = {~0U, ~0U, ~0U, ~0U};
    rendering::occlusion::write_occlusion_control(7, AmbientOcclusionSettings{}, words);
    CY_CHECK_EQ(words[0], 7U);
    CY_CHECK_EQ(words[1], 0U);  // off by default: `rendering-post-processing`
    AmbientOcclusionSettings artistic;
    artistic.apply_to_direct = true;
    artistic.direct_strength = 0.25F;
    rendering::occlusion::write_occlusion_control(7, artistic, words);
    CY_CHECK_EQ(words[1], 1U);
    f32 strength = 0.0F;
    std::memcpy(&strength, &words[2], sizeof(strength));
    CY_CHECK_EQ(strength, 0.25F);
}

CY_TEST_CASE("a settings block the search cannot run is refused") {
    const CornerScene scene = make_corner_scene(false, kWidth, kHeight);
    GtaoSettings settings;
    settings.shared.radius = 0.0F;
    CY_CHECK_FALSE(make_gtao_constants(settings, scene.view).has_value());
    settings = GtaoSettings{};
    settings.slices = 0;
    CY_CHECK_FALSE(make_gtao_constants(settings, scene.view).has_value());
    settings.slices = 3;  // not an even share of the eight lattice directions
    CY_CHECK_FALSE(make_gtao_constants(settings, scene.view).has_value());
    rendering::occlusion::GtaoView empty;
    CY_CHECK_FALSE(make_gtao_constants(GtaoSettings{}, empty).has_value());
}
