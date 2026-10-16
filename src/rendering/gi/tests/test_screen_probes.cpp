// SPDX-License-Identifier: MIT
// The screen-probe final gather on the host: the oracle its device passes transcribe, and the
// properties it is built on. Issue #35, the follow-up of stage 4.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS
// ================================================================================================
//
//   placement     a uniform probe a tile, and adaptive probes where a depth or normal step makes
//   the
//                 uniform ones unusable — and nowhere else: none over a flat floor, and every one
//                 within a tile of a step a test finds by its own definition;
//   history       a still view's probes carry their count up to `history_frames`; an invalidated
//                 region and a cut carry none; a moving camera still finds most; and a probe's
//                 blend is exactly last frame's coefficients moved toward this frame's estimate
//                 by max(1 / (count + 1), 1 / history_frames);
//   the lever     `GiGather::PerPixel` is `resolve_screen_pixel` bit for bit, the per-pixel gather
//   as
//                 it was; `ScreenProbes` is the probes' integration bit for bit, and a different
//                 image;
//
// The quality the guide quotes — the path tracer, a 10 cm wall, a moving light, a still view — is
// smoke.render_gi_screen_probes on the host and render.gi_gpu_resolve at 1080p on the device.

#include <cy/test/test.h>

#include <cy/rendering/gi/screen_probes.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/raytracing/acceleration.h>
#include <cy/rendering/raytracing/geometry.h>

#include "screen_scene.h"
#include "screen_support.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

using cy::f32;
using cy::f64;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

using gi_support::AxisQuad;
using gi_support::make_room;
using gi_support::ScreenScene;

constexpr u32 kViewWidth = 128;
constexpr u32 kViewHeight = 72;
constexpr u32 kSpacing = 16;

/// A floor seen from above, all of the view, and optionally a 50 cm block standing on it.
[[nodiscard]] std::vector<AxisQuad> floor_quads(bool with_block) {
    std::vector<AxisQuad> quads;
    quads.push_back({1, -1.0F, {-50.0F, -50.0F}, {50.0F, 50.0F}, {0.6F, 0.6F, 0.6F}, 1.0F});
    if (with_block) {
        quads.push_back({1, -0.5F, {-0.5F, -1.2F}, {0.5F, -0.2F}, {0.6F, 0.6F, 0.6F}, 1.0F});
        quads.push_back({0, -0.5F, {-1.0F, -1.2F}, {-0.5F, -0.2F}, {0.6F, 0.6F, 0.6F}, -1.0F});
        quads.push_back({0, 0.5F, {-1.0F, -1.2F}, {-0.5F, -0.2F}, {0.6F, 0.6F, 0.6F}, 1.0F});
        quads.push_back({2, -1.2F, {-0.5F, -1.0F}, {0.5F, -0.5F}, {0.6F, 0.6F, 0.6F}, -1.0F});
        quads.push_back({2, -0.2F, {-0.5F, -1.0F}, {0.5F, -0.5F}, {0.6F, 0.6F, 0.6F}, 1.0F});
    }
    return quads;
}

[[nodiscard]] gi_support::ScreenView floor_view(bool with_block) {
    return gi_support::render_view(floor_quads(with_block), Vec3{0.3F, 2.2F, 0.4F},
                                   Vec3{0.0F, -1.0F, -0.7F}, kViewWidth, kViewHeight);
}

/// A step, by this test's own definition rather than the placement's: a neighbour off the pixel's
/// plane by more than 2 % of its depth, a neighbour whose normal turns by more than 25 degrees, or
/// a neighbour that is sky.
[[nodiscard]] std::vector<bool> steps_of(const gi_support::ScreenView& view) {
    std::vector<bool> step(view.depth.size(), false);
    for (u32 y = 0; y < view.height; ++y) {
        for (u32 x = 0; x < view.width; ++x) {
            const usize at = (static_cast<usize>(y) * view.width) + x;
            if (view.depth[at] <= 0.0F) {
                continue;
            }
            // The four neighbours, each one step along an axis, those on the view.
            const u32 neighbours[4][2] = {{x + 1U, y}, {x - 1U, y}, {x, y + 1U}, {x, y - 1U}};
            for (const auto& neighbour : neighbours) {
                const u32 nx = neighbour[0];
                const u32 ny = neighbour[1];
                if (nx >= view.width || ny >= view.height) {
                    continue;  // off the view; x - 1 at x = 0 wraps past it
                }
                const usize there = (static_cast<usize>(ny) * view.width) + nx;
                if (view.depth[there] <= 0.0F ||
                    std::abs(cy::dot(view.normal[at], view.position[there] - view.position[at])) >
                        0.02F * view.depth[at] ||
                    cy::dot(view.normal[at], view.normal[there]) < 0.9F) {
                    step[at] = true;
                }
            }
        }
    }
    return step;
}

/// Whether a step lies within `reach` pixels (each way) of `pixel`.
[[nodiscard]] bool step_near(const gi_support::ScreenView& view, const std::vector<bool>& step,
                             u32 pixel, u32 reach) {
    const int px = static_cast<int>(pixel % view.width);
    const int py = static_cast<int>(pixel / view.width);
    const int r = static_cast<int>(reach);
    for (int y = std::max(0, py - r); y <= std::min(static_cast<int>(view.height) - 1, py + r);
         ++y) {
        for (int x = std::max(0, px - r); x <= std::min(static_cast<int>(view.width) - 1, px + r);
             ++x) {
            if (step[(static_cast<usize>(y) * view.width) + static_cast<usize>(x)]) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE("adaptive probes appear where the uniform grid cannot interpolate, and nowhere else") {
    ScreenProbeSettings settings;
    settings.spacing_pixels = kSpacing;

    // A flat floor: every uniform probe stands on the one plane every pixel does, so no cell needs
    // more than the grid.
    const gi_support::ScreenView flat = floor_view(false);
    for (const f32 depth : flat.depth) {
        CY_REQUIRE(depth > 0.0F);
    }
    ScreenProbeGather gather;
    CY_REQUIRE(gather.resize(kViewWidth, kViewHeight, kSpacing).has_value());
    CY_REQUIRE(gather.place(flat.surfaces(), settings).has_value());
    const ScreenProbeLayout& layout = gather.layout();
    CY_CHECK_EQ(gather.stats().uniform, layout.uniform_count());
    CY_CHECK_EQ(gather.stats().adaptive, 0U);

    // A block on the floor: its silhouette is a depth step and its edges are creases.
    const gi_support::ScreenView block = floor_view(true);
    const std::vector<bool> step = steps_of(block);
    u32 step_pixels = 0;
    for (const bool at : step) {
        step_pixels += at ? 1U : 0U;
    }
    CY_REQUIRE(step_pixels > 0U);
    CY_REQUIRE(gather.place(block.surfaces(), settings).has_value());
    u32 adaptive = 0;
    u32 beside_a_step = 0;
    for (u32 cell = 0; cell < layout.cell_count(); ++cell) {
        const ScreenProbe* probe = gather.adaptive(cell);
        if (probe == nullptr) {
            continue;
        }
        adaptive += 1;
        // The probe stands on the cell's centre, and a step lies within a tile of it: the
        // interpolation it repairs spans one tile each way.
        const u32 pixel = layout.adaptive_pixel(cell);
        CY_CHECK_EQ(probe->id, layout.uniform_count() + cell);
        CY_CHECK_EQ(probe->depth, block.depth[pixel]);
        beside_a_step += step_near(block, step, pixel, layout.spacing) ? 1U : 0U;
    }
    // Where there is no step within a tile, no cell took a probe: the flat parts of the same
    // view are as the flat floor's.
    u32 flat_cells = 0;
    for (u32 cell = 0; cell < layout.cell_count(); ++cell) {
        const u32 pixel = layout.adaptive_pixel(cell);
        if (!step_near(block, step, pixel, layout.spacing)) {
            flat_cells += 1;
            CY_CHECK(gather.adaptive(cell) == nullptr);
        }
    }
    std::fprintf(
        stderr,
        "a block on a floor, %ux%u at %u px: %u step pixels, %u of %u half-tile cells took "
        "an adaptive probe, all %u beside a step; %u cells with no step near took none\n",
        kViewWidth, kViewHeight, kSpacing, step_pixels, adaptive, layout.cell_count(),
        beside_a_step, flat_cells);
    CY_CHECK_GT(adaptive, 0U);
    CY_CHECK_EQ(beside_a_step, adaptive);
    CY_CHECK_GT(flat_cells, 0U);
    CY_CHECK_EQ(gather.stats().refused, 0U);
}

CY_TEST_CASE("an adaptive probe repairs the interpolation where the grid fails") {
    // Each pixel of the block's view interpolated from the uniform probes alone, against the
    // probes with the adaptive ones: the pixels the grid leaves without a probe on their own
    // surface are the ones the adaptive probes are for.
    const gi_support::ScreenView block = floor_view(true);
    ScreenProbeSettings settings;
    settings.spacing_pixels = kSpacing;
    ScreenProbeGather gather;
    CY_REQUIRE(gather.resize(kViewWidth, kViewHeight, kSpacing).has_value());
    CY_REQUIRE(gather.place(block.surfaces(), settings).has_value());
    const ScreenProbeLayout& layout = gather.layout();
    const cy::Span<const ScreenProbe> probes = gather.probes();
    u32 orphaned = 0;
    u32 repaired = 0;
    for (u32 pixel = 0; pixel < kViewWidth * kViewHeight; ++pixel) {
        const u32 x = pixel % kViewWidth;
        const u32 y = pixel / kViewWidth;
        // The uniform probes of the four tiles about the pixel.
        f32 grid_weight = 0.0F;
        f32 all_weight = 0.0F;
        for (u32 tile = 0; tile < layout.uniform_count(); ++tile) {
            const u32 at = layout.uniform_pixel(tile);
            const u32 dx = std::max(at % kViewWidth, x) - std::min(at % kViewWidth, x);
            const u32 dy = std::max(at / kViewWidth, y) - std::min(at / kViewWidth, y);
            if (dx >= layout.spacing || dy >= layout.spacing || probes[tile].depth <= 0.0F) {
                continue;
            }
            grid_weight += screen_probe_geometry_weight(
                probes[tile].position, probes[tile].normal, block.position[pixel],
                block.normal[pixel], block.depth[pixel], settings.plane_tolerance);
        }
        all_weight = grid_weight;
        for (u32 cell = 0; cell < layout.cell_count(); ++cell) {
            const ScreenProbe* probe = gather.adaptive(cell);
            if (probe == nullptr) {
                continue;
            }
            const u32 at = layout.adaptive_pixel(cell);
            const u32 dx = std::max(at % kViewWidth, x) - std::min(at % kViewWidth, x);
            const u32 dy = std::max(at / kViewWidth, y) - std::min(at / kViewWidth, y);
            if (dx >= layout.cell || dy >= layout.cell) {
                continue;
            }
            all_weight += screen_probe_geometry_weight(
                probe->position, probe->normal, block.position[pixel], block.normal[pixel],
                block.depth[pixel], settings.plane_tolerance);
        }
        if (grid_weight <= 0.0F) {
            orphaned += 1;
            repaired += all_weight > 0.0F ? 1U : 0U;
        }
    }
    std::fprintf(stderr,
                 "pixels no uniform probe about them stands for: %u; with the adaptive probes, %u "
                 "of them have one\n",
                 orphaned, repaired);
    CY_CHECK_GT(orphaned, 0U);
    CY_CHECK_GT(repaired * 10U, orphaned * 9U);
}

namespace {

/// The room at 64x40 with probes every 8 pixels, the specular signal off.
void probe_room(ScreenScene& scene, f32 importance = 0.5F) {
    scene.settings.gather = GiGather::ScreenProbes;
    scene.settings.probes.spacing_pixels = 8;
    scene.settings.probes.importance_fraction = importance;
    scene.specular_enabled = false;
    make_room(scene, 64, 40);
}

[[nodiscard]] u32 live_probes(const ScreenProbeGather& gather) {
    return gather.stats().uniform + gather.stats().adaptive;
}

[[nodiscard]] std::vector<const ScreenProbe*> live(const ScreenProbeGather& gather) {
    std::vector<const ScreenProbe*> out;
    const u32 records = gather.layout().uniform_count() + gather.stats().adaptive;
    for (u32 index = 0; index < records; ++index) {
        if (gather.probes()[index].depth > 0.0F) {
            out.push_back(&gather.probes()[index]);
        }
    }
    return out;
}

}  // namespace

CY_TEST_CASE(
    "a probe's history is last frame's probes where it lands, and none where invalidated") {
    ScreenScene scene;
    probe_room(scene);
    const u32 frames = scene.settings.probes.history_frames;
    scene.step(0);
    for (const ScreenProbe* probe : live(scene.resolve.probes())) {
        CY_CHECK_EQ(probe->history, 1.0F);
    }
    CY_CHECK_EQ(scene.resolve.probes().stats().reprojected, 0U);

    // A still view: every probe finds itself, and its count climbs to the cap.
    scene.step(1);
    CY_CHECK_EQ(scene.resolve.probes().stats().reprojected, live_probes(scene.resolve.probes()));
    for (const ScreenProbe* probe : live(scene.resolve.probes())) {
        CY_CHECK_EQ(probe->history, 2.0F);
    }
    for (u64 frame = 2; frame < 2 + frames; ++frame) {
        scene.step(frame);
    }
    for (const ScreenProbe* probe : live(scene.resolve.probes())) {
        CY_CHECK_EQ(probe->history, static_cast<f32>(frames));
    }

    // A light moved over the floor's near half: the probes there start over, and only there.
    const cy::Aabb floor_half =
        cy::Aabb::from_min_max(Vec3{-5.0F, -2.5F, -5.0F}, Vec3{5.0F, -1.9F, 0.0F});
    scene.invalidated.push_back(floor_half);
    scene.step(10);
    u32 restarted = 0;
    for (const ScreenProbe* probe : live(scene.resolve.probes())) {
        const bool inside = probe->position.y <= -1.9F && probe->position.z <= 0.0F;
        CY_CHECK_EQ(probe->history, inside ? 1.0F : static_cast<f32>(frames));
        restarted += inside ? 1U : 0U;
    }
    CY_CHECK_GT(restarted, 0U);

    // A cut: no history anywhere.
    scene.previous_depth.clear();
    scene.step(11);
    CY_CHECK_EQ(scene.resolve.probes().stats().reprojected, 0U);

    // A camera that slides 20 cm still finds most of its probes where they were.
    scene.step(12);
    const Vec3 eye{2.5F, 0.4F, -3.4F};
    const Vec3 target{-1.5F, -0.9F, 0.2F};
    const Vec3 right =
        cy::normalized_or(cy::cross(target - eye, Vec3{0.0F, 1.0F, 0.0F}), Vec3{1.0F, 0.0F, 0.0F});
    scene.view =
        gi_support::render_view(scene.quads, eye + (right * 0.2F), target + (right * 0.2F), 64, 40);
    scene.step(13);
    const u32 found = scene.resolve.probes().stats().reprojected;
    const u32 placed = live_probes(scene.resolve.probes());
    std::fprintf(stderr, "a 20 cm slide: %u of %u probes found a history\n", found, placed);
    CY_CHECK_GT(found * 10U, placed * 8U);
}

CY_TEST_CASE("a probe's blend moves last frame's coefficients toward this frame's estimate") {
    // With no guide, a probe's rays do not depend on its history, so the estimate a frame makes
    // is the one the same frame makes with the history forgotten; and in a still view the previous
    // probe that stands for a probe best where it lands is itself — so the blend is checkable
    // exactly.
    ScreenScene scene;
    probe_room(scene, 0.0F);
    // Eight frames of history, so frame 3's blend is its count's (1/4) and not the floor's.
    scene.settings.probes.history_frames = 8;
    // The probe-space filter reaching its neighbours, as it does at 1080p: the history blended is
    // still the probe's own coefficients, never the filtered ones it hands the pixels.
    scene.settings.probes.filter_reach = 1.0F;
    scene.run(0, 3);
    const ScreenProbeGather& gather = scene.resolve.probes();
    std::vector<ScreenProbe> before(gather.probes().begin(), gather.probes().end());
    scene.step(3);
    std::vector<ScreenProbe> blended(gather.probes().begin(), gather.probes().end());
    // This frame again, its history forgotten: the bare estimate.
    scene.resolve.reset_probe_history();
    const ScreenSurfaces surfaces = scene.grid.surfaces();
    CY_REQUIRE(scene.resolve.produce(surfaces, scene.settings, scene.sources(3)).has_value());
    const cy::Span<const ScreenProbe> estimate = gather.probes();

    const ScreenProbeLayout& layout = gather.layout();
    const f32 frames = static_cast<f32>(scene.settings.probes.history_frames);
    u32 checked = 0;
    f64 worst = 0.0;
    for (u32 tile = 0; tile < layout.uniform_count(); ++tile) {
        if (before[tile].depth <= 0.0F) {
            continue;
        }
        const f32 alpha = std::max(1.0F / (before[tile].history + 1.0F), 1.0F / frames);
        CY_CHECK_EQ(before[tile].history, 3.0F);
        CY_CHECK_EQ(blended[tile].history, std::min(before[tile].history + 1.0F, frames));
        for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
            const Vec3 expected =
                before[tile].coefficients[k] +
                ((estimate[tile].coefficients[k] - before[tile].coefficients[k]) * alpha);
            worst = std::max(
                worst, static_cast<f64>(cy::length(blended[tile].coefficients[k] - expected)));
        }
        checked += 1;
    }
    std::fprintf(stderr,
                 "%u probes: the blend within %.2g of history + (estimate - history) * 1/4\n",
                 checked, worst);
    CY_CHECK_GT(checked, 20U);
    CY_CHECK_LT(worst, 1.0e-5);
}

namespace {

/// The probe-space filter's tent at reach one, by offset -1, 0, 1: stated here rather than read off
/// the implementation, so a filter that weighed its neighbours otherwise disagrees with it.
constexpr f32 kTent[3] = {0.5F, 1.0F, 0.5F};

/// What `filter` should make of `probe`, computed from the definition: its own coefficients at
/// weight one, and each uniform neighbour of the 3x3 tiles about it by the tent, by its distance
/// against the reach, and by how well the neighbour stands for the probe's surface.
void expected_filtered(const ScreenProbeGather& gather, const ScreenProbe& probe,
                       const ScreenProbeSettings& settings, Vec3 out[kScreenProbeCoefficients]) {
    const ScreenProbeLayout& layout = gather.layout();
    const u32 uniform = layout.uniform_count();
    const u32 pixel = probe.id < uniform ? layout.uniform_pixel(probe.id)
                                         : layout.adaptive_pixel(probe.id - uniform);
    const u32 tile_x = std::min((pixel % layout.width) / layout.spacing, layout.tiles_x - 1U);
    const u32 tile_y = std::min((pixel / layout.width) / layout.spacing, layout.tiles_y - 1U);
    Vec3 sum[kScreenProbeCoefficients];
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        sum[k] = probe.coefficients[k];
    }
    f32 total = 1.0F;
    for (u32 j = 0; j < 3; ++j) {
        for (u32 i = 0; i < 3; ++i) {
            const u32 x = tile_x + i - 1U;  // wraps past the grid at the left and top edges
            const u32 y = tile_y + j - 1U;
            if (x >= layout.tiles_x || y >= layout.tiles_y) {
                continue;
            }
            const u32 tile = (y * layout.tiles_x) + x;
            const ScreenProbe& neighbour = gather.probes()[tile];
            if (tile == probe.id || neighbour.depth <= 0.0F) {
                continue;
            }
            const f32 near = 1.0F - (cy::length(neighbour.position - probe.position) /
                                     (settings.filter_reach * probe.depth));
            const f32 weight =
                kTent[i] * kTent[j] * std::max(near, 0.0F) *
                screen_probe_geometry_weight(neighbour.position, neighbour.normal, probe.position,
                                             probe.normal, probe.depth, settings.plane_tolerance);
            for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
                sum[k] = sum[k] + (neighbour.coefficients[k] * weight);
            }
            total += weight;
        }
    }
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        out[k] = sum[k] / total;
    }
}

[[nodiscard]] bool filtered_is_coefficients(const ScreenProbe& probe) {
    for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
        if (probe.filtered[k].x != probe.coefficients[k].x ||
            probe.filtered[k].y != probe.coefficients[k].y ||
            probe.filtered[k].z != probe.coefficients[k].z) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE("the probe-space filter averages a probe with its neighbours on its own surface") {
    ScreenScene scene;
    probe_room(scene);
    CY_CHECK(scene.settings.probes.denoiser == ScreenProbeDenoiser::ProbeSpace);
    CY_CHECK_EQ(scene.settings.probes.filter_tiles, 1U);
    // At 64x40 a tile is half a metre of the room's far wall, past the default reach (a tenth of
    // the depth) of every neighbour; a reach of a whole depth lets the tiles and the surfaces
    // decide.
    scene.settings.probes.filter_reach = 1.0F;
    scene.run(0, 3);
    const ScreenProbeGather& gather = scene.resolve.probes();
    u32 checked = 0;
    u32 averaged = 0;
    f64 worst = 0.0;
    for (const ScreenProbe* probe : live(gather)) {
        Vec3 expected[kScreenProbeCoefficients];
        expected_filtered(gather, *probe, scene.settings.probes, expected);
        const f64 scale = std::max(static_cast<f64>(cy::length(expected[0])), 1.0e-6);
        for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
            worst = std::max(
                worst, static_cast<f64>(cy::length(probe->filtered[k] - expected[k])) / scale);
        }
        averaged += filtered_is_coefficients(*probe) ? 0U : 1U;
        checked += 1;
    }
    std::fprintf(stderr,
                 "%u probes, %u of them averaged with a neighbour: the filter within %.2g of the "
                 "tent-and-surface average\n",
                 checked, averaged, worst);
    CY_CHECK_GT(checked, 20U);
    CY_CHECK_GT(averaged * 2U, checked);
    CY_CHECK_LT(worst, 1.0e-5);

    // The default reach at this resolution: a probe is averaged only with neighbours within a
    // tenth of its depth, and the definition above agrees with what it did.
    ScreenResolveSettings within_reach = scene.settings;
    within_reach.probes.filter_reach = ScreenProbeSettings{}.filter_reach;
    scene.resolve.reset_probe_history();
    CY_REQUIRE(
        scene.resolve.produce(scene.grid.surfaces(), within_reach, scene.sources(3)).has_value());
    u32 kept_apart = 0;
    for (const ScreenProbe* probe : live(scene.resolve.probes())) {
        Vec3 expected[kScreenProbeCoefficients];
        expected_filtered(scene.resolve.probes(), *probe, within_reach.probes, expected);
        for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
            CY_CHECK_LT(static_cast<f64>(cy::length(probe->filtered[k] - expected[k])),
                        1.0e-5 * std::max(1.0, static_cast<f64>(cy::length(expected[0]))));
        }
        kept_apart += filtered_is_coefficients(*probe) ? 1U : 0U;
    }
    std::fprintf(stderr, "at a tenth of the depth, %u probes had no neighbour within reach\n",
                 kept_apart);
    CY_CHECK_GT(kept_apart, 0U);

    // The pixels read the filtered coefficients: a probe's own are the history, never filtered,
    // so the filter does not compound frame over frame (the blend case above holds that). The same
    // frame twice from no history, unfiltered and filtered, so only the filter differs.
    const ScreenSurfaces surfaces = scene.grid.surfaces();
    ScreenResolveSettings unfiltered = scene.settings;
    unfiltered.probes.filter_tiles = 0;
    scene.resolve.reset_probe_history();
    CY_REQUIRE(scene.resolve.produce(surfaces, unfiltered, scene.sources(3)).has_value());
    const std::vector<Vec3> plain(scene.resolve.diffuse().begin(), scene.resolve.diffuse().end());
    for (const ScreenProbe* probe : live(scene.resolve.probes())) {
        CY_CHECK(filtered_is_coefficients(*probe));
    }
    scene.resolve.reset_probe_history();
    CY_REQUIRE(scene.resolve.produce(surfaces, scene.settings, scene.sources(3)).has_value());
    u32 surfaced = 0;
    u32 moved = 0;
    for (usize index = 0; index < plain.size(); ++index) {
        if (surfaces.depth[index] <= 0.0F) {
            continue;
        }
        surfaced += 1;
        const Vec3 value = scene.resolve.diffuse()[index];
        moved += value.x != plain[index].x || value.y != plain[index].y || value.z != plain[index].z
                     ? 1U
                     : 0U;
    }
    // Most probes were averaged with a neighbour, so most pixels read something else.
    std::fprintf(stderr, "filtered, %u of %u pixels integrate something else\n", moved, surfaced);
    CY_CHECK_GT(moved * 2U, surfaced);
}

CY_TEST_CASE("under the shared denoiser the probes are integrated unfiltered, as they were") {
    ScreenScene scene;
    probe_room(scene);
    scene.settings.probes.denoiser = ScreenProbeDenoiser::Shared;
    scene.run(0, 3);
    u32 checked = 0;
    for (const ScreenProbe* probe : live(scene.resolve.probes())) {
        CY_CHECK(filtered_is_coefficients(*probe));
        checked += 1;
    }
    CY_CHECK_GT(checked, 20U);
    CY_CHECK_EQ(std::string(screen_probe_denoiser_name(ScreenProbeDenoiser::Shared)),
                std::string("shared"));
    CY_CHECK_EQ(std::string(screen_probe_denoiser_name(ScreenProbeDenoiser::ProbeSpace)),
                std::string("probe space"));
}

CY_TEST_CASE("below a ray density of one, that fraction of the probes trace and the rest keep") {
    // A still view, so the previous probe that stands for each probe best is itself (the blend
    // case's argument), and a probe that does not trace keeps exactly what it had.
    ScreenScene scene;
    probe_room(scene, 0.0F);
    scene.settings.probes.history_frames = 8;
    // Everything in the far field, so a probe's openness is one of the field's sky rays rather
    // than the near field's one: what a kept probe keeps is then something a traced one is not.
    scene.settings.error_targets.far_field_begins_metres = 0.0F;
    scene.run(0, 3);
    const ScreenProbeGather& gather = scene.resolve.probes();
    const std::vector<ScreenProbe> before(gather.probes().begin(), gather.probes().end());
    scene.settings.ray_density = 0.5F;
    scene.step(3);
    const ScreenProbeLayout& layout = gather.layout();
    u32 traced = 0;
    u32 kept = 0;
    for (u32 tile = 0; tile < layout.uniform_count(); ++tile) {
        const ScreenProbe& now = gather.probes()[tile];
        if (now.depth <= 0.0F) {
            continue;
        }
        if (screen_probe_traces(now.id, 3, 0.5F)) {
            CY_CHECK_EQ(now.history, std::min(before[tile].history + 1.0F, 8.0F));
            traced += 1;
            continue;
        }
        CY_CHECK_EQ(now.history, before[tile].history);
        CY_CHECK_EQ(now.openness, before[tile].openness);
        CY_CHECK_EQ(now.cache_confidence, before[tile].cache_confidence);
        bool same = true;
        for (u32 k = 0; k < kScreenProbeCoefficients; ++k) {
            same = same && now.coefficients[k].x == before[tile].coefficients[k].x &&
                   now.coefficients[k].y == before[tile].coefficients[k].y &&
                   now.coefficients[k].z == before[tile].coefficients[k].z;
        }
        CY_CHECK(same);
        kept += 1;
    }
    std::fprintf(stderr, "at half the ray density: %u uniform probes traced, %u kept\n", traced,
                 kept);
    CY_CHECK_GT(traced, 5U);
    CY_CHECK_GT(kept, 5U);
    CY_CHECK_GE(gather.stats().carried, kept);
    CY_CHECK_EQ(gather.stats().rays,
                (live_probes(gather) - gather.stats().carried) * screen_probe_rays(scene.settings));
    // At a density of one every probe traces, whatever the frame.
    for (u32 id = 0; id < 64; ++id) {
        CY_CHECK(screen_probe_traces(id, 3, 1.0F));
    }
}

CY_TEST_CASE("the gather lever selects the per-pixel gather as it was, or the screen probes") {
    ScreenScene scene;
    scene.specular_enabled = false;
    make_room(scene);
    scene.run(0, 4);
    const ScreenSurfaces surfaces = scene.grid.surfaces();
    const usize pixels = scene.view.depth.size();

    // The per-pixel gather is `resolve_screen_pixel` at every pixel, bit for bit.
    ScreenResolveSettings per_pixel = scene.settings;
    per_pixel.gather = GiGather::PerPixel;
    ScreenResolveSources sources = scene.sources(4);
    CY_REQUIRE(scene.resolve.produce(surfaces, per_pixel, sources).has_value());
    const std::vector<Vec3> old_path(scene.resolve.diffuse().begin(),
                                     scene.resolve.diffuse().end());
    u32 same = 0;
    u32 surfaced = 0;
    for (usize index = 0; index < pixels; ++index) {
        if (surfaces.depth[index] <= 0.0F) {
            continue;
        }
        surfaced += 1;
        const Vec3 offset = surfaces.position[index] - surfaces.camera;
        const ScreenPixel pixel = resolve_screen_pixel(
            static_cast<u32>(index), surfaces.position[index],
            cy::normalized_or(surfaces.normal[index], Vec3{0.0F, 1.0F, 0.0F}),
            surfaces.roughness[index], cy::length(offset), offset, per_pixel, sources);
        same += pixel.diffuse.radiance.x == old_path[index].x &&
                        pixel.diffuse.radiance.y == old_path[index].y &&
                        pixel.diffuse.radiance.z == old_path[index].z
                    ? 1U
                    : 0U;
    }
    CY_CHECK_EQ(same, surfaced);

    // The default is the screen probes, and its frame is the probes' integration, bit for bit.
    CY_CHECK(ScreenResolveSettings{}.gather == GiGather::ScreenProbes);
    ScreenResolveSettings probes = scene.settings;
    probes.gather = GiGather::ScreenProbes;
    CY_REQUIRE(scene.resolve.produce(surfaces, probes, sources).has_value());
    CY_CHECK_GT(scene.resolve.probes().stats().uniform, 0U);
    u32 integrated = 0;
    f64 apart = 0.0;
    for (usize index = 0; index < pixels; ++index) {
        if (surfaces.depth[index] <= 0.0F) {
            continue;
        }
        const Vec3 offset = surfaces.position[index] - surfaces.camera;
        const ScreenPixel pixel = scene.resolve.probes().integrate(
            static_cast<u32>(index), surfaces.position[index],
            cy::normalized_or(surfaces.normal[index], Vec3{0.0F, 1.0F, 0.0F}),
            surfaces.roughness[index], surfaces.depth[index], cy::length(offset), offset, probes,
            sources);
        const Vec3 value = scene.resolve.diffuse()[index];
        integrated += pixel.diffuse.radiance.x == value.x && pixel.diffuse.radiance.y == value.y &&
                              pixel.diffuse.radiance.z == value.z
                          ? 1U
                          : 0U;
        apart += static_cast<f64>(cy::length(value - old_path[index]));
    }
    CY_CHECK_EQ(integrated, surfaced);
    CY_CHECK_GT(apart, 0.0);
    // The denoiser: under the probe-space denoiser the probes' filtered signal is accumulated over
    // time, its history cut where the signal leaves it, and filtered no further; under the shared
    // one, one a-trous pass and no cut; the per-pixel gather's with the cascade as authored.
    const cy::rendering::denoise::SignalConfig authored =
        cy::rendering::denoise::default_config(cy::rendering::denoise::SignalKind::IndirectDiffuse);
    CY_CHECK_EQ(authored.history_rejection, 0.0F);
    CY_CHECK(probes.probes.denoiser == ScreenProbeDenoiser::ProbeSpace);
    CY_CHECK_EQ(screen_signal_config(authored, probes).max_passes, 0U);
    CY_CHECK_EQ(screen_signal_config(authored, probes).history_length, authored.history_length);
    CY_CHECK_EQ(screen_signal_config(authored, probes).history_rejection,
                probes.probes.history_rejection);
    CY_CHECK_GT(probes.probes.history_rejection, 0.0F);
    ScreenResolveSettings shared = probes;
    shared.probes.denoiser = ScreenProbeDenoiser::Shared;
    CY_CHECK_EQ(screen_signal_config(authored, shared).max_passes, 1U);
    CY_CHECK_EQ(screen_signal_config(authored, shared).history_rejection, 0.0F);
    CY_CHECK_EQ(screen_signal_config(authored, per_pixel).max_passes, authored.max_passes);
    CY_CHECK_EQ(screen_signal_config(authored, per_pixel).history_rejection, 0.0F);
    CY_CHECK_GT(authored.max_passes, 1U);
    CY_CHECK_EQ(std::string(gi_gather_name(GiGather::ScreenProbes)), std::string("screen probes"));
    CY_CHECK_EQ(std::string(gi_gather_name(GiGather::PerPixel)), std::string("per pixel"));
}

namespace {

/// The sky's mean cosine-weighted radiance about `normal`, by quadrature: the irradiance (over pi)
/// a surface facing `normal` under an open sky and over the sky's ground receives.
[[nodiscard]] f64 sky_irradiance(const SkyTerm& sky, Vec3 normal) {
    Vec3 tangent;
    Vec3 bitangent;
    screen_basis(normal, tangent, bitangent);
    f64 total = 0.0;
    constexpr u32 kSteps = 128;
    for (u32 i = 0; i < kSteps; ++i) {
        for (u32 j = 0; j < kSteps; ++j) {
            const f32 z = (static_cast<f32>(i) + 0.5F) / kSteps;
            const f32 phi = 6.28318530718F * (static_cast<f32>(j) + 0.5F) / kSteps;
            const f32 r = std::sqrt(1.0F - (z * z));
            const Vec3 direction =
                (tangent * (r * std::cos(phi))) + (bitangent * (r * std::sin(phi))) + (normal * z);
            total += static_cast<f64>(gi_support::luminance(sky.radiance(direction))) *
                     static_cast<f64>(z);
        }
    }
    return total * 2.0 / (static_cast<f64>(kSteps) * kSteps);
}

}  // namespace

CY_TEST_CASE(
    "a pixel no uniform probe stands for reads its own surface's light from an adaptive one") {
    // The block on the floor under an open sky, with no geometry in the field: every ray escapes,
    // so a probe's answer is the sky about its own normal, and a pixel's is the sky about ITS
    // normal only if it integrated a probe of its own surface. The pixels measured are the ones no
    // uniform probe within a tile stands for — the block's narrow sides and the floor at its foot,
    // the adaptive probes' work — and every other pixel is reported beside them.
    const gi_support::ScreenView view = floor_view(true);
    DistanceField field;
    ClipmapSettings clipmap;
    clipmap.levels = 1;
    clipmap.resolution = 16;
    clipmap.base_extent_metres = 8.0F;
    CY_REQUIRE(field.configure(clipmap).has_value());
    (void)field.scroll_to(view.camera);
    const SoftwareTracer tracer(field, nullptr);
    ScreenHistoryInputs history;
    history.current_world_to_clip = view.world_to_clip;
    history.previous_world_to_clip = view.world_to_clip;
    history.previous_depth = {view.depth.data(), view.depth.size()};
    ScreenResolveSources sources;
    sources.field = &field;
    sources.software = &tracer;
    sources.history = &history;
    ScreenResolveSettings settings;
    settings.probes.spacing_pixels = kSpacing;
    settings.probes.rays = kScreenProbeMaxRays;
    settings.probes.history_frames = 8;
    ScreenProbeGather gather;
    CY_REQUIRE(gather.resize(kViewWidth, kViewHeight, kSpacing).has_value());
    for (u64 frame = 0; frame < 8; ++frame) {
        sources.frame = frame;
        CY_REQUIRE(gather.place(view.surfaces(), settings.probes).has_value());
        gather.trace(view.surfaces(), settings, sources);
    }
    const ScreenProbeLayout& layout = gather.layout();
    const cy::Span<const ScreenProbe> probes = gather.probes();
    // The sky about each face's normal, once.
    const Vec3 normals[] = {Vec3{0.0F, 1.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F},
                            Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, -1.0F}};
    f64 exact[std::size(normals)] = {};
    for (usize index = 0; index < std::size(normals); ++index) {
        exact[index] = sky_irradiance(sources.sky, normals[index]);
    }
    f64 orphan_error = 0.0;
    f64 other_error = 0.0;
    u32 orphans = 0;
    u32 others = 0;
    for (u32 pixel = 0; pixel < kViewWidth * kViewHeight; ++pixel) {
        const Vec3 normal = view.normal[pixel];
        usize face = std::size(normals);
        for (usize index = 0; index < std::size(normals); ++index) {
            face = cy::dot(normals[index], normal) > 0.99F ? index : face;
        }
        if (view.depth[pixel] <= 0.0F || face == std::size(normals)) {
            continue;
        }
        // Whether a uniform probe within a tile stands on this pixel's surface.
        f32 uniform = 0.0F;
        for (u32 tile = 0; tile < layout.uniform_count(); ++tile) {
            const u32 at = layout.uniform_pixel(tile);
            const u32 dx = std::max(at % kViewWidth, pixel % kViewWidth) -
                           std::min(at % kViewWidth, pixel % kViewWidth);
            const u32 dy = std::max(at / kViewWidth, pixel / kViewWidth) -
                           std::min(at / kViewWidth, pixel / kViewWidth);
            if (dx < layout.spacing && dy < layout.spacing && probes[tile].depth > 0.0F) {
                uniform += screen_probe_geometry_weight(
                    probes[tile].position, probes[tile].normal, view.position[pixel], normal,
                    view.depth[pixel], settings.probes.plane_tolerance);
            }
        }
        const Vec3 offset = view.position[pixel] - view.camera;
        const ScreenPixel answer =
            gather.integrate(pixel, view.position[pixel], normal, 0.8F, view.depth[pixel],
                             cy::length(offset), offset, settings, sources);
        const f64 error =
            std::abs(static_cast<f64>(gi_support::luminance(answer.diffuse.radiance)) -
                     exact[face]) /
            exact[face];
        if (uniform <= 0.0F) {
            orphan_error += error;
            orphans += 1;
        } else {
            other_error += error;
            others += 1;
        }
    }
    orphan_error /= std::max(orphans, 1U);
    other_error /= std::max(others, 1U);
    std::fprintf(stderr,
                 "under an open sky: %u pixels no uniform probe stands for read %.3f from the sky "
                 "about their own normal, the other %u %.3f\n",
                 orphans, orphan_error, others, other_error);
    // A side's hemisphere is half sky and half ground with a step at the horizon, which nine
    // coefficients round by a few per cent; a side read from the floor's probes is half its light
    // out.
    CY_CHECK_GT(orphans, 100U);
    CY_CHECK_LT(orphan_error, 0.08);
    CY_CHECK_LT(other_error, 0.08);
}

CY_TEST_CASE("a probe's estimate is unbiased whatever share of its rays the guide draws") {
    // Probes on a floor under an open sky: every ray escapes, so each answers the sky in its own
    // direction and the exact answer is the sky's mean cosine-weighted radiance about the floor's
    // normal, by quadrature. The guide is the probes' own previous estimate (a still view, one
    // frame of history), and a mixture's density that disagreed with how its rays are drawn would
    // move the mean with the guided share. A share above 0.9 is held at 0.9: a guide alone never
    // draws a cell its weight is zero in, and was measured 2.8 % dark here.
    const gi_support::ScreenView view = floor_view(false);
    DistanceField field;
    ClipmapSettings clipmap;
    clipmap.levels = 1;
    clipmap.resolution = 16;
    clipmap.base_extent_metres = 8.0F;
    CY_REQUIRE(field.configure(clipmap).has_value());
    (void)field.scroll_to(view.camera);
    const SoftwareTracer tracer(field, nullptr);
    ScreenHistoryInputs history;
    history.current_world_to_clip = view.world_to_clip;
    history.previous_world_to_clip = view.world_to_clip;
    history.previous_depth = {view.depth.data(), view.depth.size()};
    ScreenResolveSources sources;
    sources.field = &field;
    sources.software = &tracer;
    sources.history = &history;

    const Vec3 up{0.0F, 1.0F, 0.0F};
    f64 exact = 0.0;
    constexpr u32 kSteps = 256;
    for (u32 i = 0; i < kSteps; ++i) {
        for (u32 j = 0; j < kSteps; ++j) {
            const f32 z = (static_cast<f32>(i) + 0.5F) / kSteps;
            const f32 phi = 6.28318530718F * (static_cast<f32>(j) + 0.5F) / kSteps;
            const f32 r = std::sqrt(1.0F - (z * z));
            const Vec3 direction{r * std::cos(phi), z, r * std::sin(phi)};
            exact += static_cast<f64>(gi_support::luminance(sources.sky.radiance(direction))) *
                     static_cast<f64>(z);
        }
    }
    // (1/pi) * sum L z dz dphi, with dz dphi = (1/n) (2 pi / n): 2 / n^2.
    exact *= 2.0 / (static_cast<f64>(kSteps) * kSteps);

    const f32 irradiance_bands[3] = {1.0F, 2.0F / 3.0F, 0.25F};
    for (const f32 guided : {0.0F, 0.5F, 1.0F}) {
        ScreenResolveSettings settings;
        settings.probes.spacing_pixels = 8;
        settings.probes.importance_fraction = guided;
        settings.probes.history_frames = 1;
        ScreenProbeGather gather;
        CY_REQUIRE(gather.resize(kViewWidth, kViewHeight, 8).has_value());
        f64 mean = 0.0;
        u32 probes = 0;
        for (u64 frame = 0; frame < 16; ++frame) {
            sources.frame = frame;
            CY_REQUIRE(gather.place(view.surfaces(), settings.probes).has_value());
            gather.trace(view.surfaces(), settings, sources);
            if (frame == 0) {
                continue;  // no guide yet: the guided share needs a history
            }
            CY_CHECK_EQ(gather.stats().reprojected, gather.stats().uniform);
            for (u32 tile = 0; tile < gather.layout().uniform_count(); ++tile) {
                mean += static_cast<f64>(gi_support::luminance(screen_probe_evaluate(
                    gather.probes()[tile].coefficients, up, irradiance_bands)));
                probes += 1;
            }
        }
        mean /= probes;
        const f64 relative = std::abs(mean - exact) / exact;
        std::fprintf(stderr,
                     "an open sky, %u probe estimates with %.0f%% of their rays asked guided: %.4f "
                     "against the sky's %.4f (%.2g relative)\n",
                     probes, static_cast<double>(guided * 100.0F), mean, exact, relative);
        CY_CHECK_LT(relative, 0.01);
    }
}

namespace {

/// The lit room and the sealed one either side of a 10 cm wall, as integration.render_gi_screen
/// builds them, with the wall and the room in the acceleration service too.
struct SealedRooms {
    ScreenScene scene;
    std::vector<Vec3> positions;
    std::vector<u32> indices;

    void build() {
        scene.quads = gi_support::room_quads(gi_support::kRoomX, gi_support::kRoomY,
                                             gi_support::kRoomZ, false);
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
        scene.build(Vec3{2.5F, 0.3F, 3.6F}, Vec3{-1.0F, -0.5F, 0.2F}, 16, 8, true);
        for (const AxisQuad& quad : scene.quads) {
            const u32 a = quad.axis == 0 ? 1U : 0U;
            const u32 b = quad.axis == 2 ? 1U : 2U;
            const auto corner = [&](f32 u, f32 v) {
                f32 at[3] = {0.0F, 0.0F, 0.0F};
                at[quad.axis] = quad.coordinate;
                at[a] = u;
                at[b] = v;
                return Vec3{at[0], at[1], at[2]};
            };
            const auto base = static_cast<u32>(positions.size());
            positions.push_back(corner(quad.lo[0], quad.lo[1]));
            positions.push_back(corner(quad.hi[0], quad.lo[1]));
            positions.push_back(corner(quad.hi[0], quad.hi[1]));
            positions.push_back(corner(quad.lo[0], quad.hi[1]));
            indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        cy::rendering::rt::TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        CY_REQUIRE(scene.system.acceleration()
                       .declare_geometry(1, cy::rendering::rt::adapt_static_mesh(triangles, 0))
                       .has_value());
        cy::rendering::rt::InstanceDescriptor instance;
        instance.geometry = 1;
        instance.instance_id = 1;
        CY_REQUIRE(scene.system.acceleration().add_instance(instance).has_value());
        (void)scene.system.acceleration().update();
        for (u64 frame = 0; frame < 6; ++frame) {
            FrameContext context;
            context.camera = scene.view.camera;
            context.lights = {scene.lights.data(), scene.lights.size()};
            context.frame = frame;
            (void)scene.system.update(context);
        }
    }
};

}  // namespace

CY_TEST_CASE("a probe at a thin wall's foot or on a room's edge sees nothing past the surface") {
    // Probes where the sealed room's frame put them at 1080p and lit them from the other room or
    // the sky: on the floor two centimetres from the 10 cm wall (the software tier stepped inside
    // the wall and read the lit side's card), and on the edges where a side wall meets the ceiling
    // and the floor meets the back wall (the hardware tier's rays left the room through the plane
    // the probe's lift kept them on), and where a side wall, the floor and the 10 cm wall meet
    // (one step out cleared the floor and not the wall). The sealed room has no light and no
    // opening, so every ray from them answers zero; no cache stands in, so nothing hides a leak.
    SealedRooms rooms;
    rooms.build();
    struct Spot {
        const char* name = "";
        Vec3 position{0.0F, 0.0F, 0.0F};
        Vec3 normal{0.0F, 1.0F, 0.0F};
    };
    const Spot spots[] = {
        {"the floor at the wall's foot", Vec3{0.169F, -2.0F, 0.069F}, Vec3{0.0F, 1.0F, 0.0F}},
        {"the floor at the wall's foot, further along", Vec3{2.501F, -2.0F, 0.081F},
         Vec3{0.0F, 1.0F, 0.0F}},
        {"a side wall's edge at the ceiling", Vec3{-4.0F, 2.0F, 3.265F}, Vec3{1.0F, 0.0F, 0.0F}},
        {"the floor's edge at the back wall", Vec3{-1.798F, -2.0F, 4.0F}, Vec3{0.0F, 1.0F, 0.0F}},
        {"a side wall where the floor and the 10 cm wall meet it", Vec3{-4.0F, -1.99F, 0.074F},
         Vec3{1.0F, 0.0F, 0.0F}},
    };
    // One probe a spot: a 2x2-cell grid at a spacing of two holds one uniform probe, at (1, 1).
    constexpr u32 kSide = 2;
    for (const bool hardware : {false, true}) {
        ScreenResolveSources sources = rooms.scene.sources(6);
        sources.cache = nullptr;
        sources.hardware = hardware ? &rooms.scene.system.hardware_tracer() : nullptr;
        const bool tier_ready = !hardware || sources.hardware->available();
        CY_REQUIRE(tier_ready);
        ScreenResolveSettings settings;
        settings.max_ray_distance_metres = rooms.scene.settings.max_ray_distance_metres;
        settings.probes.rays = kScreenProbeMaxRays;
        for (const Spot& spot : spots) {
            constexpr usize kCells = static_cast<usize>(kSide) * kSide;
            std::vector<f32> depth(kCells, 1.0F);
            std::vector<Vec3> position(kCells, spot.position);
            std::vector<Vec3> normal(kCells, spot.normal);
            ScreenSurfaces surfaces;
            surfaces.width = kSide;
            surfaces.height = kSide;
            surfaces.depth = {depth.data(), depth.size()};
            surfaces.position = {position.data(), position.size()};
            surfaces.normal = {normal.data(), normal.size()};
            surfaces.camera = rooms.scene.view.camera;
            ScreenProbeGather gather;
            CY_REQUIRE(gather.resize(kSide, kSide, kSide).has_value());
            CY_REQUIRE(gather.place(surfaces, settings.probes).has_value());
            gather.trace(surfaces, settings, sources);
            const f32 bands[3] = {1.0F, 1.0F, 1.0F};
            f32 seen = 0.0F;
            for (u32 k = 0; k < 64; ++k) {
                const u32 row = k / 8U;
                const u32 column = k % 8U;
                const f32 z = (static_cast<f32>(row) + 0.5F) / 8.0F;
                const f32 turn = (static_cast<f32>(column) + 0.5F) / 8.0F;
                Vec3 tangent;
                Vec3 bitangent;
                screen_basis(spot.normal, tangent, bitangent);
                const f32 r = std::sqrt(1.0F - (z * z));
                const Vec3 direction = (tangent * (r * std::cos(6.2831853F * turn))) +
                                       (bitangent * (r * std::sin(6.2831853F * turn))) +
                                       (spot.normal * z);
                seen = std::max(seen, gi_support::luminance(screen_probe_evaluate(
                                          gather.probes()[0].coefficients, direction, bands)));
            }
            std::fprintf(stderr, "%s tier, %s: the brightest radiance it holds %.3g\n",
                         hardware ? "hardware" : "software", spot.name, static_cast<double>(seen));
            CY_CHECK_EQ(seen, 0.0F);
        }
    }
}
