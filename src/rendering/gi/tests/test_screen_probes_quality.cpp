// SPDX-License-Identifier: MIT
// What the screen-probe gather's image is worth on the host, beside the per-pixel gather's, on the
// guide's four axes: the path tracer, a 10 cm wall, a moving light and a still view. Issue #35.
//
// SMOKE, because each case converges a scene per gather and the path tracer's reference is a few
// hundred points of three bounces; ONE CASE A SPACING AND AN AXIS, because a Debug build is about
// eight times slower and each case is held to its own budget. The view is 128x72: large enough that
// a 16-pixel tile is a tile and not a fifth of the room, small enough for the host. The per-pixel
// gather is measured beside the probes against the path tracer; its 10 cm wall, still view and
// moved light are integration.render_gi_screen's and smoke.render_gi_screen_convergence's. The
// 1080p and 4K numbers, through both tiers, are smoke.gi_gpu_probe_quality's on the device, which
// render.gi_gpu_resolve holds to this oracle to a few parts in ten million.

#include <cy/test/test.h>

#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/gi/system.h>

#include "screen_scene.h"
#include "screen_support.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using cy::f32;
using cy::f64;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

using gi_support::image_difference;
using gi_support::luminance;
using gi_support::make_room;
using gi_support::ScreenScene;

constexpr u32 kWidth = 128;
constexpr u32 kHeight = 72;

/// One gather the axes are measured for.
struct Variant {
    const char* name;
    GiGather gather;
    u32 spacing;
};

constexpr Variant kPerPixel{"per-pixel gather", GiGather::PerPixel, 16};
constexpr Variant kProbes16{"screen probes every 16 px", GiGather::ScreenProbes, 16};
constexpr Variant kProbes8{"screen probes every 8 px", GiGather::ScreenProbes, 8};

void configure(ScreenScene& scene, const Variant& variant) {
    scene.settings.gather = variant.gather;
    scene.settings.probes.spacing_pixels = variant.spacing;
    scene.specular_enabled = false;
}

/// A converged frame of the GI test room through `variant` against the path tracer at the same
/// 260-odd points; its frame for the next comparison.
[[nodiscard]] f32 reference_error(const Variant& variant, std::vector<Vec3>& frame_out) {
    ScreenScene scene;
    configure(scene, variant);
    make_room(scene, kWidth, kHeight);
    scene.run(0, 40);
    gi_support::QuadTracer exact(scene.quads);
    const PathTracer reference(exact, scene.system.scene(),
                               {scene.lights.data(), scene.lights.size()}, scene.system.sky_term(),
                               &exact);
    BakeSettings bake;
    bake.bounces = 3;
    bake.samples = 192;
    std::vector<Vec3> truth;
    std::vector<Vec3> frame;
    for (u32 y = 1; y < kHeight; y += 6) {
        for (u32 x = 1; x < kWidth; x += 6) {
            const usize index = (static_cast<usize>(y) * kWidth) + x;
            if (scene.view.depth[index] <= 0.0F) {
                continue;
            }
            truth.push_back(
                reference.irradiance(scene.view.position[index], scene.view.normal[index], bake));
            frame.push_back(scene.diffuse[index]);
        }
    }
    const ReferenceComparison measured =
        compare_against_reference({frame.data(), frame.size()}, {truth.data(), truth.size()});
    std::fprintf(
        stderr, "GI test room %ux%u, %s: %.3f relative to the path tracer over %u points\n", kWidth,
        kHeight, variant.name, static_cast<double>(measured.relative_error), measured.samples);
    CY_CHECK_GT(measured.samples, 200U);
    CY_CHECK_LT(measured.relative_error, 0.16F);
    frame_out = scene.copy_diffuse();
    return measured.relative_error;
}

/// The probes against the path tracer and against the per-pixel frame, beside the per-pixel
/// gather's own error. The goal is no worse than the per-pixel gather: at 1080p both spacings are
/// within 0.002 of it (smoke.gi_gpu_probe_quality); here a 16-pixel tile is an eighth of the view's
/// width and a probe stands for a metre of wall, so the margin is the spacing's.
void against_per_pixel(const Variant& probes, f32 margin) {
    std::vector<Vec3> per_pixel_frame;
    std::vector<Vec3> probe_frame;
    const f32 per_pixel = reference_error(kPerPixel, per_pixel_frame);
    const f32 measured = reference_error(probes, probe_frame);
    const f32 from_per_pixel = [&] {
        f64 total = 0.0;
        f64 scale = 0.0;
        for (usize index = 0; index < probe_frame.size(); ++index) {
            total += static_cast<f64>(cy::length(probe_frame[index] - per_pixel_frame[index]));
            scale += static_cast<f64>(cy::length(per_pixel_frame[index]));
        }
        return scale > 0.0 ? static_cast<f32>(total / scale) : 0.0F;
    }();
    std::fprintf(stderr, "%s: %.3f from the per-pixel frame\n", probes.name,
                 static_cast<double>(from_per_pixel));
    CY_CHECK_LT(measured, per_pixel + margin);
    CY_CHECK_LT(from_per_pixel, 0.15F);
}

void sealed_room(ScreenScene& scene, const Variant& variant, Vec3 eye, Vec3 target) {
    configure(scene, variant);
    scene.quads =
        gi_support::room_quads(gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ, false);
    scene.quads.push_back({2,
                           -0.05F,
                           {-gi_support::kRoomX, -gi_support::kRoomY},
                           {gi_support::kRoomX, gi_support::kRoomY},
                           {0.6F, 0.6F, 0.6F},
                           -1.0F});
    scene.quads.push_back({2,
                           0.05F,
                           {-gi_support::kRoomX, -gi_support::kRoomY},
                           {gi_support::kRoomX, gi_support::kRoomY},
                           {0.6F, 0.6F, 0.6F},
                           1.0F});
    scene.surfels = gi_support::surfels_from_quads(scene.quads, 1.0F);
    scene.add_solid(Vec3{0.0F, 0.0F, 0.0F}, Vec3{gi_support::kRoomX, gi_support::kRoomY, 0.05F},
                    43);
    scene.lights[0].position = Vec3{-1.0F, 0.5F, -1.2F};
    scene.build(eye, target, kWidth, kHeight);
}

void behind_the_wall(const Variant& variant) {
    ScreenScene lit;
    sealed_room(lit, variant, Vec3{2.5F, 0.3F, -3.6F}, Vec3{-1.0F, -0.5F, -0.2F});
    ScreenScene sealed;
    sealed_room(sealed, variant, Vec3{2.5F, 0.3F, 3.6F}, Vec3{-1.0F, -0.5F, 0.2F});
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
                 "10 cm wall, %s: the sealed room's mean %.2e of the lit room's, its worst pixel "
                 "%.2e\n",
                 variant.name, sealed_mean / lit_mean, sealed_worst / lit_mean);
    CY_REQUIRE(lit_mean > 0.0);
    CY_CHECK_GT(sealed_count, 4000U);
    CY_CHECK_LT(sealed_mean / lit_mean, 1.0e-3);
    CY_CHECK_LT(sealed_worst / lit_mean, 2.0e-2);
}

void still_view(const Variant& variant) {
    ScreenScene scene;
    configure(scene, variant);
    make_room(scene, kWidth, kHeight);
    scene.run(0, 40);
    const std::vector<Vec3> previous = scene.copy_diffuse();
    const std::vector<Vec3> previous_noisy(scene.resolve.diffuse().begin(),
                                           scene.resolve.diffuse().end());
    scene.step(40);
    const f32 frame_to_frame = image_difference(scene.copy_diffuse(), previous, scene.view.depth);
    const std::vector<Vec3> noisy(scene.resolve.diffuse().begin(), scene.resolve.diffuse().end());
    const f32 noisy_to_noisy = image_difference(noisy, previous_noisy, scene.view.depth);
    std::fprintf(stderr,
                 "still view, %s: the frame moves %.4f a frame, the signal under the denoiser "
                 "%.3f\n",
                 variant.name, static_cast<double>(frame_to_frame),
                 static_cast<double>(noisy_to_noisy));
    CY_CHECK_LT(frame_to_frame, 0.01F);
}

void moved_light(const Variant& variant) {
    const Vec3 moved{2.4F, 1.2F, -2.6F};
    ScreenScene settled;
    configure(settled, variant);
    make_room(settled, kWidth, kHeight);
    settled.lights[0].position = moved;
    settled.run(0, 48);
    const std::vector<Vec3> target = settled.copy_diffuse();

    ScreenScene scene;
    configure(scene, variant);
    make_room(scene, kWidth, kHeight);
    scene.run(0, 32);
    const f32 before = image_difference(scene.copy_diffuse(), target, scene.view.depth);
    const cy::Aabb room = cy::Aabb::from_center_extents(
        Vec3{0.0F, 0.0F, 0.0F},
        Vec3{gi_support::kRoomX + 0.5F, gi_support::kRoomY + 0.5F, gi_support::kRoomZ + 0.5F});
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
                 "a light moved 4.9 m, %s: %.3f from the new light's converged frame before, "
                 "within %.2f after %u frames (%.3f)\n",
                 variant.name, static_cast<double>(before), static_cast<double>(kWithin), frames,
                 static_cast<double>(difference));
    CY_CHECK_GT(before, 0.3F);
    CY_CHECK_LE(difference, kWithin);
    CY_CHECK_LE(frames, 14U);
}

}  // namespace

CY_TEST_CASE("converged, the probes every 16 px are near the path tracer, beside the per-pixel") {
    against_per_pixel(kProbes16, 0.03F);
}

CY_TEST_CASE("converged, the probes every 8 px are near the path tracer, beside the per-pixel") {
    against_per_pixel(kProbes8, 0.02F);
}

CY_TEST_CASE("through the probes every 16 px, no light reaches the sealed room behind 10 cm") {
    behind_the_wall(kProbes16);
}

CY_TEST_CASE("through the probes every 8 px, no light reaches the sealed room behind 10 cm") {
    behind_the_wall(kProbes8);
}

CY_TEST_CASE("through the probes every 16 px, a still view of a converged room is still") {
    still_view(kProbes16);
}

CY_TEST_CASE("through the probes every 8 px, a still view of a converged room is still") {
    still_view(kProbes8);
}

CY_TEST_CASE("through the probes every 16 px, the frame settles on a moved light in 14 frames") {
    moved_light(kProbes16);
}

CY_TEST_CASE("through the probes every 8 px, the frame settles on a moved light in 14 frames") {
    moved_light(kProbes8);
}
