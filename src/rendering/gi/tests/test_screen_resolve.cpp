// The per-pixel resolve and its denoiser on the host: the oracle issue #35's stage 4 transcribes to
// the device, and the quality bars the frame's GI is held to.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS, AND WHY HERE RATHER THAN ON THE DEVICE
// ================================================================================================
//
// `render.gi_gpu_resolve` holds the device resolve and denoiser to `resolve_screen_pixel` and
// `denoise::Denoiser` pixel for pixel, to a few parts in ten million. So a property of the image
// this file measures on the host is a property of the device's image to that bound — and this file
// runs on every machine continuous integration has, where the device suite has no device. The
// numbers it prints are the ones the README and the guide quote:
//
//   the resolve        a pixel takes the radiance cache, one final-gather ray and the far-field sky
//                      by confidence, under `exclusion_for()`'s mask;
//   the history        `temporal-rendering`'s reprojection, and an invalidated region keeps none;
//   ground truth       converged, the frame is measured against the path tracer over the EXACT
//                      geometry (`QuadTracer`) — not the field the tiers trace — beside what the
//                      probes alone (`IlluminationSystem::indirect_diffuse`) answer;
//   a 10 cm wall       no light from a lit room reaches the sealed one beside it;
//   a light that moves the frames until the image is within a stated distance of the new light's
//                      converged image;
//   a still view       how much a converged frame moves from one frame to the next.

#include <cy/test/test.h>

#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/gi/system.h>

#include "screen_scene.h"
#include "screen_support.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using cy::f32;
using cy::f64;
using cy::i32;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)
namespace denoise = cy::rendering::denoise;

using gi_support::image_difference;
using gi_support::kScreenHeight;
using gi_support::kScreenWidth;
using gi_support::luminance;
using gi_support::make_room;
using gi_support::ScreenScene;

constexpr u32 kWidth = kScreenWidth;
constexpr u32 kHeight = kScreenHeight;

}  // namespace

CY_TEST_CASE("a pixel takes the cache, one ray and the far sky, combined by confidence") {
    ScreenScene scene;
    make_room(scene);
    scene.run(0, 6);
    const u32 pixel = (kHeight / 2U * kWidth) + (kWidth / 2U);
    CY_REQUIRE(scene.view.depth[pixel] > 0.0F);
    const Vec3 position = scene.view.position[pixel];
    const Vec3 normal = scene.view.normal[pixel];
    const Vec3 offset = position - scene.view.camera;

    // With no ray to spend and the pixel in the near field, the answer IS the radiance cache's.
    ScreenResolveSettings cache_only = scene.settings;
    cache_only.ray_density = 0.0F;
    const ScreenPixel cached = resolve_screen_pixel(pixel, position, normal, 0.8F, cy::length(offset),
                                                    offset, cache_only, scene.sources(6));
    // The diffuse answer reads no reflection direction; any will do.
    const RadianceSample sample =
        scene.system.radiance()
            .sample_traced(position, normal, normal, 6, scene.system.field())
            .diffuse;
    CY_CHECK_FALSE(cached.traced_diffuse);
    CY_CHECK_GT(sample.confidence, 0.0F);
    CY_CHECK_EQ(cached.diffuse.radiance.x, sample.radiance.x);
    CY_CHECK_EQ(cached.diffuse.radiance.y, sample.radiance.y);
    CY_CHECK_EQ(cached.diffuse.radiance.z, sample.radiance.z);
    CY_CHECK_EQ(cached.diffuse.sources_used, source_bit(RadianceSource::RadianceCache));

    // With a ray, the pixel's own view of the room is combined with the lattice's, not chosen.
    const ScreenPixel traced = resolve_screen_pixel(pixel, position, normal, 0.8F,
                                                    cy::length(offset), offset, scene.settings,
                                                    scene.sources(6));
    CY_CHECK(traced.traced_diffuse);
    CY_CHECK_NE(traced.diffuse.sources_used & source_bit(RadianceSource::RadianceCache), 0U);
    CY_CHECK_NE(traced.diffuse.sources_used & source_bit(RadianceSource::SoftwareTrace), 0U);

    // A mode that admits no dynamic bounce takes none: the closed room has no far sky, so nothing.
    ScreenResolveSettings none = scene.settings;
    none.mode = GiMode::None;
    const ScreenPixel excluded = resolve_screen_pixel(pixel, position, normal, 0.8F,
                                                      cy::length(offset), offset, none,
                                                      scene.sources(6));
    CY_CHECK_EQ(excluded.diffuse.sources_used, 0U);
    CY_CHECK_EQ(cy::length(excluded.diffuse.radiance), 0.0F);

    // The ray density is the `TracingResolution` lever's: a quarter of the pixels trace.
    ScreenResolveSettings quarter = scene.settings;
    quarter.ray_density = 0.25F;
    CY_REQUIRE(scene.resolve.produce(scene.view.surfaces(), quarter, scene.sources(7)).has_value());
    u32 surfaces = 0;
    for (const f32 depth : scene.view.depth) {
        surfaces += depth > 0.0F ? 1U : 0U;
    }
    const f32 fraction = static_cast<f32>(scene.resolve.traced()) / static_cast<f32>(surfaces);
    std::fprintf(stderr, "ray density 0.25: %u of %u pixels traced (%.3f)\n",
                 scene.resolve.traced(), surfaces, static_cast<double>(fraction));
    CY_CHECK_GT(fraction, 0.18F);
    CY_CHECK_LT(fraction, 0.32F);
}

namespace {

/// Every card the wall has, shaded long ago: a bright answer the tracer distrusts as stale.
class StaleWall final : public RadianceLookup {
public:
    static constexpr u32 kAge = 240;
    [[nodiscard]] bool radiance_at(Vec3 /*position*/, Vec3 /*normal*/, Vec3& radiance,
                                   u32& age_frames) const noexcept override {
        radiance = Vec3{40.0F, 30.0F, 20.0F};
        age_frames = kAge;
        return true;
    }
};

}  // namespace

CY_TEST_CASE("a final-gather ray counts in full whatever it met: a stale wall does not lose to the sky") {
    // A floor point beside a tall wall, open to the sky: about a third of its rays meet the wall,
    // whose every card is stale, and the rest escape. Far enough from the camera that the far
    // sky is a second sample of the pixel beside the ray — the combine the ray's weight decides.
    DistanceField field;
    ClipmapSettings clipmap;
    clipmap.levels = 2;
    clipmap.resolution = 32;
    clipmap.base_extent_metres = 8.0F;
    CY_REQUIRE(field.configure(clipmap).has_value());
    const gi_support::BoxField wall(Vec3{0.5F, 4.0F, 4.0F}, 0.5F, 17);
    CY_REQUIRE(field.place(1, wall.asset(), cy::Mat4::from_translation(Vec3{1.5F, 0.0F, 0.0F}))
                   .has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    const StaleWall cards;
    const SoftwareTracer tracer(field, &cards);
    ScreenResolveSources sources;
    sources.field = &field;
    sources.software = &tracer;
    ScreenResolveSettings settings;
    settings.ray_density = 1.0F;
    const Vec3 position{0.0F, -0.5F, 0.0F};
    const Vec3 normal{0.0F, 1.0F, 0.0F};
    const f32 camera_distance = 200.0F;
    const f32 far = far_field_weight(camera_distance, settings.error_targets);
    CY_REQUIRE(far > 0.0F);

    // Each frame against what it must be: the far sky and the ray, the ray counted in full with
    // whatever value it met. The ray is drawn and traced as the resolve does.
    constexpr u32 kFrames = 512;
    f64 resolved_sum = 0.0;
    f64 expected_sum = 0.0;
    f64 worst = 0.0;
    u32 met_wall = 0;
    const f32 lift = std::max(settings.origin_lift_metres, field.voxel_size());
    for (u32 frame = 0; frame < kFrames; ++frame) {
        sources.frame = frame;
        const ScreenPixel pixel = resolve_screen_pixel(0, position, normal, 1.0F, camera_distance,
                                                       Vec3{0.0F, -1.0F, 0.0F}, settings, sources);
        const u32 hash = screen_hash(0, frame, kScreenStreamDirection);
        const Vec3 direction =
            screen_cosine_direction(normal, screen_unit(hash), screen_unit(hash * 0x27220A95U));
        SceneHit hit;
        (void)tracer.trace(position + (normal * lift), direction, settings.max_ray_distance_metres,
                           hit);
        const RadianceSample ray = tracer.radiance(hit, sources.sky, direction);
        met_wall += hit.hit ? 1U : 0U;
        const u32 count = settings.sky_visibility_rays;
        const f32 openness = field.sky_visibility_ray(position, normal, frame % count, count);
        const Vec3 sky = sources.sky.radiance(normal) * openness;
        const f32 sky_weight = std::min(far * openness, 1.0F);
        const Vec3 expected = ((ray.radiance * 1.0F) + (sky * sky_weight)) / (1.0F + sky_weight);
        const f64 difference = static_cast<f64>(cy::length(pixel.diffuse.radiance - expected)) /
                               static_cast<f64>(std::max(cy::length(expected), 1.0e-3F));
        worst = std::max(worst, difference);
        resolved_sum += static_cast<f64>(luminance(pixel.diffuse.radiance));
        expected_sum += static_cast<f64>(luminance(expected));
    }
    std::fprintf(stderr,
                 "stale wall: %u of %u rays met it; mean luminance resolved %.4f, a ray counted "
                 "in full %.4f; worst frame %.2g\n",
                 met_wall, kFrames, resolved_sum / kFrames, expected_sum / kFrames, worst);
    CY_CHECK_GT(met_wall, kFrames / 8U);
    CY_CHECK_LT(met_wall, kFrames - (kFrames / 8U));
    CY_CHECK_LT(worst, 1.0e-5);
}

CY_TEST_CASE("the history is the temporal framework's reprojection, and an invalidated region keeps none") {
    std::vector<gi_support::AxisQuad> quads =
        gi_support::room_quads(gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ, true);
    const Vec3 eye{2.5F, 0.4F, -3.4F};
    const Vec3 target{-1.5F, -0.9F, 0.2F};
    const gi_support::ScreenView still = gi_support::render_view(quads, eye, target, kWidth, kHeight);
    const usize pixels = still.depth.size();
    std::vector<i32> source(pixels, 0);
    std::vector<f32> confidence(pixels, 0.0F);

    // A still camera: every surface pixel is its own history, at full confidence.
    ScreenHistoryInputs inputs;
    inputs.current_world_to_clip = still.world_to_clip;
    inputs.previous_world_to_clip = still.world_to_clip;
    inputs.previous_depth = {still.depth.data(), still.depth.size()};
    CY_REQUIRE(screen_history(still.surfaces(), inputs, {source.data(), pixels},
                              {confidence.data(), pixels})
                   .has_value());
    u32 own = 0;
    u32 surfaces = 0;
    for (usize index = 0; index < pixels; ++index) {
        if (still.depth[index] <= 0.0F) {
            CY_CHECK_EQ(source[index], -1);
            continue;
        }
        surfaces += 1;
        own += source[index] == static_cast<i32>(index) && confidence[index] == 1.0F ? 1U : 0U;
    }
    CY_CHECK_EQ(own, surfaces);

    // The camera slides 30 cm to its right: the surfaces land a pixel or two to the left of where
    // they were, and the strip the camera uncovered has no history.
    const Vec3 right = cy::normalized_or(cy::cross(target - eye, Vec3{0.0F, 1.0F, 0.0F}),
                                         Vec3{1.0F, 0.0F, 0.0F});
    const gi_support::ScreenView moved =
        gi_support::render_view(quads, eye + (right * 0.3F), target + (right * 0.3F), kWidth, kHeight);
    inputs.current_world_to_clip = moved.world_to_clip;
    CY_REQUIRE(screen_history(moved.surfaces(), inputs, {source.data(), pixels},
                              {confidence.data(), pixels})
                   .has_value());
    u32 shifted = 0;
    u32 kept = 0;
    for (usize index = 0; index < pixels; ++index) {
        if (source[index] < 0) {
            continue;
        }
        kept += 1;
        shifted += source[index] != static_cast<i32>(index) ? 1U : 0U;
    }
    CY_CHECK_GT(kept, surfaces / 2U);
    CY_CHECK_GT(shifted, kept / 2U);

    // A region invalidated this frame — a light moved there — keeps no history, and only there.
    const cy::Aabb floor_half = cy::Aabb::from_min_max(Vec3{-5.0F, -2.5F, -5.0F}, Vec3{5.0F, -1.9F, 0.0F});
    inputs.current_world_to_clip = still.world_to_clip;
    inputs.invalidated = {&floor_half, 1};
    CY_REQUIRE(screen_history(still.surfaces(), inputs, {source.data(), pixels},
                              {confidence.data(), pixels})
                   .has_value());
    u32 floor_rejected = 0;
    u32 others_kept = 0;
    u32 others = 0;
    for (usize index = 0; index < pixels; ++index) {
        if (still.depth[index] <= 0.0F) {
            continue;
        }
        const Vec3 p = still.position[index];
        const bool inside = p.y <= -1.9F && p.z <= 0.0F;
        if (inside) {
            floor_rejected += source[index] < 0 ? 1U : 0U;
            CY_CHECK_EQ(source[index], -1);
        } else {
            others += 1;
            others_kept += source[index] == static_cast<i32>(index) ? 1U : 0U;
        }
    }
    CY_CHECK_GT(floor_rejected, 0U);
    CY_CHECK_EQ(others_kept, others);

    // And a surface that is not where the history says it was — a door swung into the view — is
    // rejected by the depth test.
    std::vector<f32> nearer = still.depth;
    for (f32& depth : nearer) {
        depth *= 0.5F;
    }
    inputs.invalidated = {};
    inputs.previous_depth = {nearer.data(), nearer.size()};
    CY_REQUIRE(screen_history(still.surfaces(), inputs, {source.data(), pixels},
                              {confidence.data(), pixels})
                   .has_value());
    for (usize index = 0; index < pixels; ++index) {
        CY_CHECK_EQ(source[index], -1);
    }
}

CY_TEST_CASE("converged, the frame is nearer the path tracer than the probes alone") {
    ScreenScene scene;
    make_room(scene);
    scene.run(0, 40);

    gi_support::QuadTracer exact(scene.quads);
    const PathTracer reference(exact, scene.system.scene(), {scene.lights.data(), scene.lights.size()},
                               scene.system.sky_term(), &exact);
    BakeSettings bake;
    bake.bounces = 3;
    bake.samples = 192;
    std::vector<Vec3> truth;
    std::vector<Vec3> frame;
    std::vector<Vec3> probes;
    for (u32 y = 1; y < kHeight; y += 2) {
        for (u32 x = 1; x < kWidth; x += 2) {
            const usize index = (static_cast<usize>(y) * kWidth) + x;
            if (scene.view.depth[index] <= 0.0F) {
                continue;
            }
            const Vec3 p = scene.view.position[index];
            const Vec3 n = scene.view.normal[index];
            truth.push_back(reference.irradiance(p, n, bake));
            frame.push_back(scene.diffuse[index]);
            probes.push_back(scene.system.indirect_diffuse(p, n, SurfaceProperties{}).radiance);
        }
    }
    const ReferenceComparison resolved = compare_against_reference(
        {frame.data(), frame.size()}, {truth.data(), truth.size()});
    const ReferenceComparison cached = compare_against_reference(
        {probes.data(), probes.size()}, {truth.data(), truth.size()});
    std::fprintf(stderr,
                 "GI test room, %u points against the path tracer (3 bounces, 192 paths): "
                 "per-pixel resolve %.3f relative, probes alone %.3f\n",
                 resolved.samples, static_cast<double>(resolved.relative_error),
                 static_cast<double>(cached.relative_error));
    // MEASURED 0.123 against the probes' 0.436 (RTX 5060 host, every profile the same: it is
    // arithmetic). The bounds: under 0.16, and under half what the probes alone answer.
    CY_CHECK_GT(resolved.samples, 100U);
    CY_CHECK_LT(resolved.relative_error, 0.16F);
    CY_CHECK_LT(resolved.relative_error, cached.relative_error * 0.5F);
}

CY_TEST_CASE("on a half-resolution grid the frame stays near the path tracer") {
    // What `TracingResolution`'s coarse positions cost in accuracy: a quarter of the pixels
    // resolved and denoised, the rest upsampled by depth and normal.
    ScreenScene scene;
    make_room(scene);
    scene.divisor = 2;
    scene.resize_grid();
    scene.run(0, 40);
    gi_support::QuadTracer exact(scene.quads);
    const PathTracer reference(exact, scene.system.scene(), {scene.lights.data(), scene.lights.size()},
                               scene.system.sky_term(), &exact);
    BakeSettings bake;
    bake.bounces = 3;
    bake.samples = 192;
    std::vector<Vec3> truth;
    std::vector<Vec3> frame;
    for (u32 y = 1; y < kHeight; y += 2) {
        for (u32 x = 1; x < kWidth; x += 2) {
            const usize index = (static_cast<usize>(y) * kWidth) + x;
            if (scene.view.depth[index] <= 0.0F) {
                continue;
            }
            truth.push_back(reference.irradiance(scene.view.position[index],
                                                 scene.view.normal[index], bake));
            frame.push_back(scene.diffuse[index]);
        }
    }
    const ReferenceComparison upsampled = compare_against_reference(
        {frame.data(), frame.size()}, {truth.data(), truth.size()});
    std::fprintf(stderr,
                 "GI test room at a half-resolution grid, %u points (each between grid samples): "
                 "%.3f relative to the path tracer\n",
                 upsampled.samples, static_cast<double>(upsampled.relative_error));
    // MEASURED 0.150 against the full grid's 0.113 and the probes' 0.436.
    CY_CHECK_LT(upsampled.relative_error, 0.19F);
}

CY_TEST_CASE("no light from a lit room reaches the sealed room behind a 10 cm wall") {
    // Two sealed halves of the room and one light in the near one. The path tracer's answer for
    // the far half is exactly zero: there is no opening, so every photon in it came through the
    // wall, and every one the frame shows there is a leak.
    const auto build = [](ScreenScene& scene, Vec3 eye, Vec3 target) {
        scene.quads = gi_support::room_quads(gi_support::kRoomX, gi_support::kRoomY,
                                             gi_support::kRoomZ, false);
        scene.quads.push_back({2, -0.05F, {-gi_support::kRoomX, -gi_support::kRoomY},
                               {gi_support::kRoomX, gi_support::kRoomY}, {0.6F, 0.6F, 0.6F}, -1.0F});
        scene.quads.push_back({2, 0.05F, {-gi_support::kRoomX, -gi_support::kRoomY},
                               {gi_support::kRoomX, gi_support::kRoomY}, {0.6F, 0.6F, 0.6F}, 1.0F});
        scene.surfels = gi_support::surfels_from_quads(scene.quads, 1.0F);
        scene.add_solid(Vec3{0.0F, 0.0F, 0.0F}, Vec3{gi_support::kRoomX, gi_support::kRoomY, 0.05F},
                        43);
        scene.lights[0].position = Vec3{-1.0F, 0.5F, -1.2F};
        scene.build(eye, target);
    };
    // The lit room seen from inside it, and the sealed room seen from inside it, toward the wall.
    ScreenScene lit;
    build(lit, Vec3{2.5F, 0.3F, -3.6F}, Vec3{-1.0F, -0.5F, -0.2F});
    ScreenScene sealed;
    build(sealed, Vec3{2.5F, 0.3F, 3.6F}, Vec3{-1.0F, -0.5F, 0.2F});
    lit.run(0, 30);
    sealed.run(0, 30);

    f64 lit_total = 0.0;
    f64 sealed_total = 0.0;
    f64 sealed_worst = 0.0;
    u32 lit_count = 0;
    u32 sealed_count = 0;
    for (usize index = 0; index < lit.diffuse.size(); ++index) {
        if (lit.view.depth[index] > 0.0F) {
            lit_total += static_cast<f64>(luminance(lit.diffuse[index]));
            lit_count += 1;
        }
        if (sealed.view.depth[index] > 0.0F && sealed.view.position[index].z > 0.05F) {
            const auto value = static_cast<f64>(luminance(sealed.diffuse[index]));
            sealed_total += value;
            sealed_worst = std::max(sealed_worst, value);
            sealed_count += 1;
        }
    }
    const f64 lit_mean = lit_total / std::max(1U, lit_count);
    const f64 sealed_mean = sealed_total / std::max(1U, sealed_count);
    std::fprintf(stderr,
                 "10 cm wall: the lit room's mean indirect %.4g, the sealed room's %.4g (%.2e of "
                 "it), worst sealed pixel %.2e of the lit mean; the path tracer says zero\n",
                 lit_mean, sealed_mean, sealed_mean / lit_mean, sealed_worst / lit_mean);
    // MEASURED: the sealed room's mean 2.6e-5 of the lit room's, its worst pixel 7.5e-3.
    CY_REQUIRE(lit_mean > 0.0);
    CY_CHECK_GT(sealed_count, 400U);
    CY_CHECK_LT(sealed_mean / lit_mean, 1.0e-3);
    CY_CHECK_LT(sealed_worst / lit_mean, 2.0e-2);
}

CY_TEST_CASE("after a light moves, the frame converges on the new light within a stated frame count") {
    // The scene converged under one light, the light moved across the room, and the region filed
    // as invalidated with the scene and with the resolve's history, as a renderer does. Measured
    // against the same scene converged under the moved light from the start.
    const Vec3 moved{2.4F, 1.2F, -2.6F};
    ScreenScene settled;
    make_room(settled);
    settled.lights[0].position = moved;
    settled.run(0, 48);
    const std::vector<Vec3> target = settled.copy_diffuse();

    ScreenScene scene;
    make_room(scene);
    scene.run(0, 32);
    const f32 before = image_difference(scene.copy_diffuse(), target, scene.view.depth);
    const cy::Aabb room = cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F},
                                                        Vec3{gi_support::kRoomX + 0.5F,
                                                             gi_support::kRoomY + 0.5F,
                                                             gi_support::kRoomZ + 0.5F});
    scene.lights[0].position = moved;
    scene.system.scene().invalidate(room, InvalidationCause::LightChanged, scene.lights[0].id, 32);
    scene.invalidated.push_back(room);
    constexpr f32 kWithin = 0.10F;
    u32 frames = 0;
    f32 difference = before;
    for (u64 frame = 32; frame < 32 + 40; ++frame) {
        scene.step(frame);
        frames += 1;
        difference = image_difference(scene.copy_diffuse(), target, scene.view.depth);
        if (difference <= kWithin) {
            break;
        }
    }
    std::fprintf(stderr,
                 "a light moved 4.9 m: %.3f from the new light's converged frame before, within "
                 "%.2f after %u frames (%.3f)\n",
                 static_cast<double>(before), static_cast<double>(kWithin), frames,
                 static_cast<double>(difference));
    // MEASURED: 11 frames.
    CY_CHECK_GT(before, 0.3F);
    CY_CHECK_LE(difference, kWithin);
    CY_CHECK_LE(frames, 14U);
}

CY_TEST_CASE("a still view of a converged room is still") {
    ScreenScene scene;
    make_room(scene);
    scene.run(0, 40);
    const std::vector<Vec3> previous = scene.copy_diffuse();
    const std::vector<Vec3> previous_noisy(scene.resolve.diffuse().begin(),
                                           scene.resolve.diffuse().end());
    scene.step(40);
    const f32 frame_to_frame = image_difference(scene.copy_diffuse(), previous, scene.view.depth);
    const std::vector<Vec3> noisy(scene.resolve.diffuse().begin(), scene.resolve.diffuse().end());
    const f32 noisy_to_noisy = image_difference(noisy, previous_noisy, scene.view.depth);
    std::fprintf(stderr,
                 "still view, converged: the frame moves %.4f a frame (the one-ray signal under it "
                 "moves %.3f)\n",
                 static_cast<double>(frame_to_frame), static_cast<double>(noisy_to_noisy));
    // MEASURED: 0.0030 a frame, the one-ray signal 0.44.
    CY_CHECK_LT(frame_to_frame, 0.01F);
    CY_CHECK_LT(frame_to_frame * 10.0F, noisy_to_noisy);
}
