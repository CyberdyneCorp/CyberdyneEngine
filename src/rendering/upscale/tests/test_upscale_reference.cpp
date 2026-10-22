// SPDX-License-Identifier: MIT
// Temporal upscaling on the host: the reference reconstruction run frame after frame over synthetic
// scenes whose every sample the case computed itself. `integration.rendering_upscale`.
//
// Each case is one of the requirement's claims about the picture, measured against the
// supersampled scene rather than against the reconstruction's own earlier output, and each has a
// CONTROL that shows the measure would see the failure it guards against: the same run with the
// mechanism that answers the claim taken away.
//
// The output is 96x54 — 16:9 and small enough that sixty-four frames of the reference fit the
// integration budget several times over on a slower host — and every mode renders at its own scale
// of it. The thin-line cases run at 64x36 instead (`kLineOutput`), for the reason given there.

#include "upscale_scene.h"

#include <cy/test/test.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace cy;
using namespace cy::upscale_test;

namespace {

constexpr Extent2D kOutput{96, 54};
constexpr u32 kConvergeFrames = 64;

[[nodiscard]] bool verbose() noexcept {
    return std::getenv("CY_UPSCALE_VERBOSE") != nullptr;
}

/// A still scene with every kind of detail an upscaler is judged on: a checker finer than the
/// render pixels of the lower modes, two slanted edges, and a bright spot.
[[nodiscard]] Surface still_scene(Vec2 uv) noexcept {
    const f32 px = uv.x * static_cast<f32>(kOutput.width);
    const f32 py = uv.y * static_cast<f32>(kOutput.height);
    Surface surface;
    const auto cell =
        static_cast<i32>(std::floor(px / 2.6F)) + static_cast<i32>(std::floor(py / 2.6F));
    surface.colour = (cell & 1) != 0 ? Vec3{0.80F, 0.70F, 0.60F} : Vec3{0.10F, 0.15F, 0.20F};
    if (px > 48.0F) {
        // The right half: a slanted edge between two flat colours.
        surface.colour = (py - 8.0F) > 0.45F * (px - 48.0F) ? Vec3{0.85F, 0.30F, 0.20F}
                                                            : Vec3{0.15F, 0.45F, 0.80F};
    }
    const f32 dx = px - 72.0F;
    const f32 dy = py - 40.0F;
    if ((dx * dx) + (dy * dy) < 9.0F) {
        surface.colour = Vec3{4.0F, 3.6F, 3.0F};
    }
    return surface;
}

/// The thin-line cases' own output: 16:9 like `kOutput` and four ninths of its pixels. Every
/// pixel a reconstruction runs over costs the same whether the line is near it or not, and at 96x54
/// two thirds of the frame is flat background the measure never reads — which put the coverage
/// case within a tenth of its Debug budget on a loaded runner. The line keeps its width, its slope
/// and its length in output pixels, and every mode keeps its scale, so the jitter cycle a case
/// measures over is the same number of phases it was at 96x54.
constexpr Extent2D kLineOutput{64, 36};
/// Where the line crosses the left edge, in output pixels from the top.
constexpr f32 kLineOffset = 6.0F;

/// A thin slanted line, 0.35 output pixels wide, over a flat background: narrower than a render
/// pixel in every mode below native, so most frames' samples miss it.
[[nodiscard]] Surface line_scene(Vec2 uv) noexcept {
    const f32 px = uv.x * static_cast<f32>(kLineOutput.width);
    const f32 py = uv.y * static_cast<f32>(kLineOutput.height);
    Surface surface;
    surface.colour = Vec3{0.12F, 0.12F, 0.14F};
    const f32 distance = std::fabs((py - kLineOffset) - (0.25F * px));
    if (distance < 0.175F * std::sqrt(1.0F + (0.25F * 0.25F))) {
        surface.colour = Vec3{1.0F, 1.0F, 1.0F};
    }
    return surface;
}

[[nodiscard]] bool near_line(u32 x, u32 y) noexcept {
    const f32 px = static_cast<f32>(x) + 0.5F;
    const f32 py = static_cast<f32>(y) + 0.5F;
    return std::fabs((py - kLineOffset) - (0.25F * px)) < 2.0F && x > 4 &&
           x + 4 < kLineOutput.width;
}

[[nodiscard]] UpscaleSettings unsharpened() noexcept {
    UpscaleSettings settings;
    settings.sharpness = 0.0F;
    return settings;
}

[[nodiscard]] Extent2D render_of(UpscaleMode mode, Extent2D output = kOutput) noexcept {
    return render_extent_for(output, upscale_mode_scale(mode));
}

}  // namespace

namespace {

/// Converge on the still scene and measure the mean absolute error per channel against the
/// 8x8-supersampled output, first frame and last.
void check_convergence(UpscaleMode mode, f32 bound) {
    const SceneFn scene = &still_scene;
    const std::vector<Vec3> native = supersample(scene, kOutput);
    HostRun run(kOutput, render_of(mode), unsharpened());
    run.frame(scene);
    const f32 first = mean_error(run.history(), native, kOutput);
    for (u32 frame = 1; frame < kConvergeFrames; ++frame) {
        run.frame(scene);
    }
    CY_REQUIRE_FALSE(run.failed());
    const f32 converged = mean_error(run.history(), native, kOutput);
    if (verbose()) {
        std::printf("converge %-12s render %ux%u  first %.4f  converged %.4f\n",
                    upscale_mode_name(mode), run.render().width, run.render().height,
                    static_cast<double>(first), static_cast<double>(converged));
    }
    CY_CHECK_LT(converged, bound);
    CY_CHECK_LT(converged, first / 2.5F);
}

}  // namespace

// THE BOUNDS, per mode, on the mean absolute error per channel against the 8x8-supersampled output
// after 64 frames, each with its measurement beside it. The floor under them is the reconstruction
// kernel itself: the picture filtered by `exp(-4 d^2)` instead of a box differs from the box's by
// about half of each. The first frame — one jittered sample a render pixel, reconstructed spatially
// — is two and a half to five times as far: accumulation must take at least three fifths away.

CY_TEST_CASE("a still scene converges to the supersampled output (native)") {
    check_convergence(UpscaleMode::NativeAntialiasing, 0.017F);
}

CY_TEST_CASE("a still scene converges to the supersampled output (quality)") {
    check_convergence(UpscaleMode::Quality, 0.020F);
}

CY_TEST_CASE("a still scene converges to the supersampled output (balanced)") {
    check_convergence(UpscaleMode::Balanced, 0.020F);
}

CY_TEST_CASE("a still scene converges to the supersampled output (performance)") {
    check_convergence(UpscaleMode::Performance, 0.021F);
}

CY_TEST_CASE("the samples are placed where the jittered projection put them") {
    // THE SIGN CONVENTION, measured rather than asserted: the same run with the sample offset
    // mirrored converges to a picture displaced by twice the jitter's spread, which a still frame
    // shows as softness. Its error is the control; the true placement must beat it by a margin.
    const SceneFn scene = &still_scene;
    const std::vector<Vec3> native = supersample(scene, kOutput);
    const Extent2D render = render_of(UpscaleMode::Performance);
    HostRun run(kOutput, render, unsharpened());
    HostRun mirrored(kOutput, render, unsharpened());
    // One whole Performance jitter cycle: every sample position, each once.
    for (u32 frame = 0; frame < jitter_phase_count(kOutput.width, render.width); ++frame) {
        const Vec2 jitter = run.next_jitter();
        (void)mirrored.next_jitter();
        const Samples samples = sample_scene(scene, render, jitter);
        run.step(samples, jitter);
        // The same samples, handed over with the opposite jitter.
        mirrored.step(samples, Vec2{-jitter.x, -jitter.y});
    }
    const f32 right = mean_error(run.history(), native, kOutput);
    const f32 wrong = mean_error(mirrored.history(), native, kOutput);
    if (verbose()) {
        std::printf("placement right %.4f mirrored %.4f\n", static_cast<double>(right),
                    static_cast<double>(wrong));
    }
    CY_CHECK_LT(right * 1.5F, wrong);
}

namespace {

constexpr f32 kLineBackground = 0.12F;
/// Past the 16-sample cap, so the cycle that follows is measured on a converged line.
constexpr u32 kLineConvergeFrames = 24;

/// The line's energy near it: every pixel's excess over the background, summed.
[[nodiscard]] f64 line_energy(const std::vector<Vec4>& image) noexcept {
    f64 energy = 0.0;
    for (u32 y = 0; y < kLineOutput.height; ++y) {
        for (u32 x = 0; x < kLineOutput.width; ++x) {
            if (near_line(x, y)) {
                energy += static_cast<f64>(
                    image[(static_cast<usize>(y) * kLineOutput.width) + x].x - kLineBackground);
            }
        }
    }
    return energy;
}

struct LineRun {
    f64 deviation = 0.0;
    f64 energy = 0.0;
};

/// Converge on the line, then measure every pixel near it over one whole jitter cycle: its temporal
/// standard deviation, averaged, and the line's energy at the end. `history` false reconstructs
/// every frame from its own samples alone — the line flickering in and out as they hit and miss it.
[[nodiscard]] LineRun run_line(UpscaleMode mode, const UpscaleSettings& settings, bool history) {
    const SceneFn scene = &line_scene;
    HostRun run(kLineOutput, render_of(mode, kLineOutput), settings);
    // Frames to converge — none without a history, which has nothing to converge — then a whole
    // jitter cycle measured.
    const u32 converge = history ? kLineConvergeFrames : 0U;
    for (u32 frame = 0; frame < converge; ++frame) {
        const Vec2 jitter = run.next_jitter();
        run.step(sample_scene(scene, run.render(), jitter), jitter, true, false, history);
    }
    const u32 cycle = jitter_phase_count(kLineOutput.width, run.render().width);
    const usize pixels = static_cast<usize>(kLineOutput.width) * kLineOutput.height;
    std::vector<f64> sum(pixels, 0.0);
    std::vector<f64> squares(pixels, 0.0);
    for (u32 frame = 0; frame < cycle; ++frame) {
        const Vec2 jitter = run.next_jitter();
        run.step(sample_scene(scene, run.render(), jitter), jitter, true, false, history);
        for (usize at = 0; at < pixels; ++at) {
            const auto value = static_cast<f64>(run.history()[at].x);
            sum[at] += value;
            squares[at] += value * value;
        }
    }
    LineRun out;
    usize count = 0;
    for (u32 y = 0; y < kLineOutput.height; ++y) {
        for (u32 x = 0; x < kLineOutput.width; ++x) {
            if (!near_line(x, y)) {
                continue;
            }
            const usize at = (static_cast<usize>(y) * kLineOutput.width) + x;
            const f64 mean = sum[at] / cycle;
            out.deviation += std::sqrt(std::fmax((squares[at] / cycle) - (mean * mean), 0.0));
            ++count;
        }
    }
    out.deviation /= static_cast<f64>(count);
    out.energy = line_energy(run.history());
    return out;
}

}  // namespace

namespace {

/// The supersampled line's energy: what full coverage is.
[[nodiscard]] f64 reference_line_energy() {
    std::vector<Vec4> native;
    for (const Vec3& texel : supersample(&line_scene, kLineOutput, 8)) {
        native.push_back(Vec4{texel.x, texel.y, texel.z, 1.0F});
    }
    return line_energy(native);
}

/// STABLE: the line does not flicker as the samples hit and miss it. Measured: deviation 0.0054
/// (quality) and 0.0095 (performance) against 0.100 and 0.118 without history.
void check_line_stable(UpscaleMode mode) {
    const LineRun locked = run_line(mode, unsharpened(), true);
    const LineRun flicker = run_line(mode, unsharpened(), false);
    if (verbose()) {
        std::printf("line %-12s deviation %.4f control %.4f\n", upscale_mode_name(mode),
                    locked.deviation, flicker.deviation);
    }
    CY_CHECK_LT(locked.deviation, 0.015);
    CY_CHECK_LT(locked.deviation * 5.0, flicker.deviation);
}

/// AND STILL THERE: the frames whose samples miss it do not clip it away — which, without the
/// locks, is most of it. Measured: 86 % (quality) and 68 % (performance) of the supersampled line's
/// energy with locks, 27 % and 15 % without.
void check_line_coverage(UpscaleMode mode, f64 kept) {
    const f64 reference = reference_line_energy();
    UpscaleSettings unlocked = unsharpened();
    unlocked.lock_thin_features = false;
    const LineRun locked = run_line(mode, unsharpened(), true);
    const LineRun eroded = run_line(mode, unlocked, true);
    if (verbose()) {
        std::printf("line %-12s energy %.1f unlocked %.1f reference %.1f\n",
                    upscale_mode_name(mode), locked.energy, eroded.energy, reference);
    }
    CY_CHECK_GT(locked.energy, reference * kept);
    CY_CHECK_GT(locked.energy, eroded.energy * 2.0);
}

}  // namespace

CY_TEST_CASE("a thin line is stable under the jitter (quality)") {
    check_line_stable(UpscaleMode::Quality);
}

CY_TEST_CASE("a thin line is stable under the jitter (performance)") {
    check_line_stable(UpscaleMode::Performance);
}

CY_TEST_CASE("a thin line keeps its coverage (quality)") {
    check_line_coverage(UpscaleMode::Quality, 0.70);
}

CY_TEST_CASE("a thin line keeps its coverage (performance)") {
    check_line_coverage(UpscaleMode::Performance, 0.50);
}

namespace {

constexpr f32 kSquareSize = 12.0F;
constexpr f32 kSquareSpeed = 2.0F;  // output pixels a frame
constexpr Vec3 kDark{0.05F, 0.05F, 0.06F};

/// A dark, softly textured, still background: texture enough that the neighbourhood box has a
/// size, as a real background does.
[[nodiscard]] Vec3 dark_background(f32 px, f32 py) noexcept {
    const f32 wave = 0.5F + (0.5F * std::sin(px * 1.3F) * std::sin(py * 0.9F));
    return Vec3{kDark.x + (0.04F * wave), kDark.y + (0.03F * wave), kDark.z + (0.02F * wave)};
}

/// A bright square moving right across the dark background, with the motion vectors the prepass
/// would write for it. `left` places it.
[[nodiscard]] Surface moving_square(Vec2 uv, f32 left) noexcept {
    const f32 px = uv.x * static_cast<f32>(kOutput.width);
    const f32 py = uv.y * static_cast<f32>(kOutput.height);
    Surface surface;
    surface.colour = dark_background(px, py);
    surface.metres = 10.0F;
    if (px >= left && px < left + kSquareSize && py >= 30.0F && py < 30.0F + kSquareSize) {
        surface.colour = Vec3{1.0F, 0.95F, 0.9F};
        surface.metres = 5.0F;
        surface.velocity = Vec2{-kSquareSpeed / static_cast<f32>(kOutput.width), 0.0F};
    }
    return surface;
}

/// How much brighter than the background the trail behind the square is, summed over the pixels
/// it left at least two frames ago, against the supersampled background.
[[nodiscard]] f64 trail_of(const std::vector<Vec4>& image, const std::vector<Vec3>& background,
                           f32 left) noexcept {
    f64 excess = 0.0;
    for (u32 y = 30; y < 42; ++y) {
        for (u32 x = 0; x < kOutput.width; ++x) {
            const f32 px = static_cast<f32>(x) + 0.5F;
            if (px >= left - (2.0F * kSquareSpeed) || px < left - 24.0F) {
                continue;
            }
            const usize at = (static_cast<usize>(y) * kOutput.width) + x;
            excess += static_cast<f64>(std::fmax(image[at].x - background[at].x, 0.0F));
        }
    }
    return excess;
}

}  // namespace

namespace {

void check_trail(UpscaleMode mode) {
    {
        HostRun run(kOutput, render_of(mode), unsharpened());
        UpscaleSettings loose = unsharpened();
        // THE CONTROL: no disocclusion test, no shading-change test and a clip box too wide to
        // clip — history taken wherever the motion points, which behind a moving object is the
        // object.
        loose.still_clip_gamma = 1000.0F;
        loose.moving_clip_gamma = 1000.0F;
        loose.detect_shading_change = false;
        loose.reject_disocclusion = false;
        HostRun control(kOutput, render_of(mode), loose);
        f32 left = 10.0F;
        for (u32 frame = 0; frame < 40; ++frame) {
            const SceneFn scene = [left](Vec2 uv) { return moving_square(uv, left); };
            const Vec2 jitter = run.next_jitter();
            (void)control.next_jitter();
            const Samples samples = sample_scene(scene, run.render(), jitter);
            run.step(samples, jitter);
            control.step(samples, jitter);
            left += kSquareSpeed;
        }
        left -= kSquareSpeed;
        const std::vector<Vec3> background = supersample(
            [](Vec2 uv) {
                Surface surface;
                surface.colour = dark_background(uv.x * static_cast<f32>(kOutput.width),
                                                 uv.y * static_cast<f32>(kOutput.height));
                return surface;
            },
            kOutput);
        const f64 trail = trail_of(run.history(), background, left);
        const f64 ghost = trail_of(control.history(), background, left);
        if (verbose()) {
            std::printf("trail %-12s %.3f control %.3f\n", upscale_mode_name(mode), trail, ghost);
        }
        // Summed over the 12 x 20 pixels behind the square: below one fully lit pixel's worth.
        CY_CHECK_LT(trail, 1.0);
        CY_CHECK_GT(ghost, (10.0 * trail) + 5.0);
    }
}

}  // namespace

CY_TEST_CASE("a moving object leaves no trail behind it (native)") {
    check_trail(UpscaleMode::NativeAntialiasing);
}

CY_TEST_CASE("a moving object leaves no trail behind it (performance)") {
    check_trail(UpscaleMode::Performance);
}

namespace {

constexpr f32 kPanelLeft = 20.0F;
constexpr f32 kPanelSize = 24.0F;
constexpr f32 kPanelSpeed = 1.25F;  // output pixels a frame

/// A finely textured panel moving right over the dark background, with its motion vectors: detail
/// finer than a Performance render pixel, which only an accumulated history can resolve.
[[nodiscard]] Surface moving_panel(Vec2 uv, f32 left) noexcept {
    const f32 px = uv.x * static_cast<f32>(kOutput.width);
    const f32 py = uv.y * static_cast<f32>(kOutput.height);
    Surface surface;
    surface.colour = dark_background(px, py);
    surface.metres = 10.0F;
    if (px >= left && px < left + kPanelSize && py >= 15.0F && py < 15.0F + kPanelSize) {
        const f32 u = px - left;
        const auto cell = static_cast<i32>(std::floor(u / 4.0F)) +
                          static_cast<i32>(std::floor((py - 15.0F) / 4.0F));
        surface.colour = (cell & 1) != 0 ? Vec3{0.85F, 0.75F, 0.55F} : Vec3{0.15F, 0.2F, 0.3F};
        surface.metres = 5.0F;
        surface.velocity = Vec2{-kPanelSpeed / static_cast<f32>(kOutput.width), 0.0F};
    }
    return surface;
}

}  // namespace

CY_TEST_CASE("a moving object keeps its detail by its motion vectors") {
    // THE CLAIM IS REPROJECTION, not rejection: a trail is prevented by rejecting the history
    // behind an object, which needs no motion at all, while the object's own texture survives its
    // motion only if its history is fetched from where the object was. The control is the same run
    // with the motion vectors withheld, reprojecting by the camera alone.
    const Extent2D render = render_of(UpscaleMode::Performance);
    HostRun moved(kOutput, render, unsharpened());
    HostRun unmoved(kOutput, render, unsharpened());
    f32 left = kPanelLeft;
    for (u32 frame = 0; frame < 40; ++frame) {
        const SceneFn scene = [left](Vec2 uv) { return moving_panel(uv, left); };
        const Vec2 jitter = moved.next_jitter();
        (void)unmoved.next_jitter();
        const Samples samples = sample_scene(scene, render, jitter);
        moved.step(samples, jitter, true);
        unmoved.step(samples, jitter, false);
        left += kPanelSpeed;
    }
    CY_REQUIRE_FALSE(moved.failed());
    CY_REQUIRE_FALSE(unmoved.failed());
    left -= kPanelSpeed;
    const std::vector<Vec3> native =
        supersample([left](Vec2 uv) { return moving_panel(uv, left); }, kOutput);
    // The panel's interior, two pixels in from its edges.
    const auto interior = [left](u32 x, u32 y) {
        const f32 px = static_cast<f32>(x) + 0.5F;
        const f32 py = static_cast<f32>(y) + 0.5F;
        return px >= left + 2.0F && px < left + kPanelSize - 2.0F && py >= 17.0F &&
               py < 15.0F + kPanelSize - 2.0F;
    };
    const f32 with_motion = mean_error(moved.history(), native, kOutput, interior);
    const f32 without = mean_error(unmoved.history(), native, kOutput, interior);
    if (verbose()) {
        std::printf("moving detail: %.4f with motion vectors, %.4f without\n",
                    static_cast<double>(with_motion), static_cast<double>(without));
    }
    CY_CHECK_LT(with_motion, 0.6F * without);
}

namespace {

/// A textured still background with a red occluder over part of it, which moves 16 pixels right
/// between two frames.
[[nodiscard]] Surface occluded(Vec2 uv, f32 occluder_left) noexcept {
    const f32 px = uv.x * static_cast<f32>(kOutput.width);
    const f32 py = uv.y * static_cast<f32>(kOutput.height);
    Surface surface;
    const f32 wave = 0.5F + (0.5F * std::sin(px * 0.9F) * std::sin(py * 0.7F));
    surface.colour = Vec3{0.2F + (0.3F * wave), (0.5F * wave) + 0.2F, 0.25F};
    surface.metres = 10.0F;
    if (px >= occluder_left && px < occluder_left + 30.0F && py >= 12.0F && py < 44.0F) {
        surface.colour = Vec3{0.95F, 0.05F, 0.05F};
        surface.metres = 4.0F;
        surface.velocity = Vec2{-16.0F / static_cast<f32>(kOutput.width), 0.0F};
    }
    return surface;
}

}  // namespace

CY_TEST_CASE("a revealed background is rebuilt from this frame, not the occluder's history") {
    const Extent2D render = render_of(UpscaleMode::Performance);
    UpscaleSettings settings = unsharpened();
    // The disocclusion test alone: the shading-change test catches a red occluder leaving a green
    // background as well, so both runs have it off and only the depth test differs between them.
    settings.detect_shading_change = false;
    HostRun run(kOutput, render, settings);
    UpscaleSettings unrejected = settings;
    unrejected.reject_disocclusion = false;
    HostRun control(kOutput, render, unrejected);
    const SceneFn before = [](Vec2 uv) { return occluded(uv, 30.0F); };
    const SceneFn after = [](Vec2 uv) { return occluded(uv, 46.0F); };
    for (u32 frame = 0; frame < 24; ++frame) {
        const Vec2 jitter = run.next_jitter();
        (void)control.next_jitter();
        const Samples samples = sample_scene(before, render, jitter);
        run.step(samples, jitter);
        control.step(samples, jitter);
    }
    const Vec2 jitter = run.next_jitter();
    (void)control.next_jitter();
    const Samples samples = sample_scene(after, render, jitter);
    run.step(samples, jitter);
    // THE CONTROL: the same frame with the disocclusion test off, so only the neighbourhood clip
    // and the shading-change test stand between the revealed pixels and the occluder's red history.
    control.step(samples, jitter);

    const std::vector<Vec3> background =
        supersample([](Vec2 uv) { return occluded(uv, 1000.0F); }, kOutput);
    // The pixels the occluder covered last frame and does not now, away from its new edge.
    const auto revealed = [](u32 x, u32 y) { return x >= 31 && x < 44 && y >= 14 && y < 42; };
    const f32 error = mean_error(run.history(), background, kOutput, revealed);
    const f32 contaminated = mean_error(control.history(), background, kOutput, revealed);
    if (verbose()) {
        std::printf("revealed error %.4f control %.4f\n", static_cast<double>(error),
                    static_cast<double>(contaminated));
    }
    // The revealed pixels are this frame's spatial reconstruction of the background: one sample a
    // render pixel of a texture at a quarter of the pixels, measured at 0.020 against 0.23 with
    // the disocclusion test off.
    CY_CHECK_LT(error, 0.05F);
    CY_CHECK_LT(error * 1.5F, contaminated);
}

CY_TEST_CASE("a change of shading reaches the picture in the frame it happens") {
    const Extent2D render = render_of(UpscaleMode::Quality);
    HostRun run(kOutput, render, unsharpened());
    // THE CONTROL: the same run with the shading-change test off, so only the clip answers it.
    UpscaleSettings clip_only = unsharpened();
    clip_only.detect_shading_change = false;
    HostRun control(kOutput, render, clip_only);
    const SceneFn dim = [](Vec2 uv) {
        Surface surface = still_scene(uv);
        surface.colour =
            Vec3{surface.colour.x * 0.2F, surface.colour.y * 0.2F, surface.colour.z * 0.2F};
        return surface;
    };
    const SceneFn lit = &still_scene;
    for (u32 frame = 0; frame < 32; ++frame) {
        const Vec2 jitter = run.next_jitter();
        (void)control.next_jitter();
        const Samples samples = sample_scene(dim, render, jitter);
        run.step(samples, jitter);
        control.step(samples, jitter);
    }
    // The light comes on.
    const Vec2 jitter = run.next_jitter();
    (void)control.next_jitter();
    const Samples samples = sample_scene(lit, render, jitter);
    run.step(samples, jitter);
    control.step(samples, jitter);
    const std::vector<Vec3> native = supersample(lit, kOutput);
    const f32 error = mean_error(run.history(), native, kOutput);
    const f32 lagging = mean_error(control.history(), native, kOutput);
    if (verbose()) {
        std::printf("shading change error %.4f control %.4f\n", static_cast<double>(error),
                    static_cast<double>(lagging));
    }
    // One frame after a fivefold change, the picture is the lit scene's spatial reconstruction
    // within its own error (the first-frame error of the convergence case, 0.065 at quality):
    // measured 0.075, against 0.31 with the clip alone.
    CY_CHECK_LT(error, 0.09F);
    CY_CHECK_LT(error * 2.0F, lagging);
}

namespace {

/// A still background with a translucent, flickering overlay in a box — a particle effect, whose
/// colour changes every frame and whose motion the velocity target does not carry.
[[nodiscard]] Surface flicker(Vec2 uv, u32 frame) noexcept {
    Surface surface = still_scene(uv);
    const f32 px = uv.x * static_cast<f32>(kOutput.width);
    const f32 py = uv.y * static_cast<f32>(kOutput.height);
    if (px >= 20.0F && px < 50.0F && py >= 20.0F && py < 50.0F) {
        surface.translucent = true;
        surface.opaque = surface.colour;
        const f32 glow = (frame & 1U) != 0U ? 0.9F : 0.1F;
        surface.colour = Vec3{surface.colour.x + glow, surface.colour.y + (glow * 0.6F),
                              surface.colour.z + (glow * 0.2F)};
    }
    return surface;
}

}  // namespace

CY_TEST_CASE("a reactive pixel follows the current frame instead of averaging it away") {
    const Extent2D render = render_of(UpscaleMode::Balanced);
    // The reactive mask alone: the shading-change test catches some of the flicker too, so both
    // runs have it off and only the mask differs between them.
    UpscaleSettings settings = unsharpened();
    settings.detect_shading_change = false;
    HostRun run(kOutput, render, settings);
    HostRun control(kOutput, render, settings);
    u32 frame = 0;
    for (; frame < 32; ++frame) {
        const SceneFn scene = [frame](Vec2 uv) { return flicker(uv, frame); };
        const Vec2 jitter = run.next_jitter();
        (void)control.next_jitter();
        const Samples samples = sample_scene(scene, render, jitter);
        run.step(samples, jitter, true, true);
        control.step(samples, jitter, true, false);
    }
    const SceneFn last = [frame](Vec2 uv) { return flicker(uv, frame - 1U); };
    const std::vector<Vec3> native = supersample(last, kOutput);
    const auto inside = [](u32 x, u32 y) { return x >= 23 && x < 47 && y >= 23 && y < 47; };
    const f32 error = mean_error(run.history(), native, kOutput, inside);
    const f32 averaged = mean_error(control.history(), native, kOutput, inside);
    if (verbose()) {
        std::printf("reactive error %.4f control %.4f\n", static_cast<double>(error),
                    static_cast<double>(averaged));
    }
    // The reactive pixels are this frame's spatial reconstruction — measured 0.130, the spatial
    // error of a balanced render of the checker under the overlay — against 0.25 averaged.
    CY_CHECK_LT(error, 0.15F);
    CY_CHECK_LT(error * 1.5F, averaged);
}

CY_TEST_CASE("with no history the reconstruction is this frame's alone") {
    const Extent2D render = render_of(UpscaleMode::Quality);
    HostRun run(kOutput, render, unsharpened());
    for (u32 frame = 0; frame < 8; ++frame) {
        run.frame(&still_scene);
    }
    // A cut: the framework says the history is invalid. Whatever the history holds, the frame is
    // the one a fresh run reconstructs from the same samples.
    const Vec2 jitter = run.next_jitter();
    const Samples samples =
        sample_scene([](Vec2 uv) { return occluded(uv, 30.0F); }, render, jitter);
    run.step(samples, jitter, true, false, false);
    HostRun fresh(kOutput, render, unsharpened());
    fresh.step(samples, jitter);
    usize differing = 0;
    for (usize at = 0; at < run.history().size(); ++at) {
        const Vec4 a = run.history()[at];
        const Vec4 b = fresh.history()[at];
        differing += (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) ? 1U : 0U;
    }
    CY_CHECK_EQ(differing, 0U);
}

CY_TEST_CASE("sharpening copies at zero and raises an edge's contrast without overshooting") {
    std::vector<Vec4> edge(static_cast<usize>(kOutput.width) * kOutput.height);
    for (u32 y = 0; y < kOutput.height; ++y) {
        for (u32 x = 0; x < kOutput.width; ++x) {
            // A soft step: dark, two intermediate columns, bright.
            constexpr f32 kStep[4] = {0.1F, 0.3F, 0.6F, 0.8F};
            const f32 value = kStep[x < 60 ? 0U : std::min(x - 59U, 3U)];
            edge[(static_cast<usize>(y) * kOutput.width) + x] = Vec4{value, value, value, 5.0F};
        }
    }
    UpscaleSettings settings;
    settings.sharpness = 0.0F;
    std::vector<Vec4> copied(edge.size());
    CY_REQUIRE(sharpen_reference(Span<const Vec4>(edge.data(), edge.size()),
                                 make_sharpen_constants(settings, kOutput),
                                 Span<Vec4>(copied.data(), copied.size()))
                   .has_value());
    for (usize at = 0; at < edge.size(); ++at) {
        CY_REQUIRE_EQ(copied[at].x, edge[at].x);
    }
    settings.sharpness = 1.0F;
    std::vector<Vec4> sharpened(edge.size());
    CY_REQUIRE(sharpen_reference(Span<const Vec4>(edge.data(), edge.size()),
                                 make_sharpen_constants(settings, kOutput),
                                 Span<Vec4>(sharpened.data(), sharpened.size()))
                   .has_value());
    const usize row = static_cast<usize>(30) * kOutput.width;
    // The ramp steepens: the dark side of the step darker, the bright side brighter.
    CY_CHECK_LT(sharpened[row + 60].x, 0.3F);
    CY_CHECK_GT(sharpened[row + 61].x, 0.6F);
    // And nothing leaves the range of the values around it, which is what RCAS's lobe limit is for.
    CY_CHECK_GE(sharpened[row + 60].x, 0.0F);
    CY_CHECK_LE(sharpened[row + 61].x, 0.8F);
    // A flat region is untouched.
    CY_CHECK_NEAR(sharpened[row + 10].x, 0.1F, 1e-5F);
    CY_CHECK_EQ(sharpened[row + 10].w, 1.0F);
}

CY_TEST_CASE("the resample takes the depth and motion of the sample nearest each output pixel") {
    const Extent2D render = render_of(UpscaleMode::Performance);
    HostRun run(kOutput, render, unsharpened());
    const Vec2 jitter = run.next_jitter();
    const SceneFn scene = [](Vec2 uv) { return moving_square(uv, 40.0F); };
    const Samples samples = sample_scene(scene, render, jitter);
    run.step(samples, jitter);
    UpscaleInputs inputs;
    inputs.color = Span<const Vec4>(samples.color.data(), samples.color.size());
    inputs.depth = Span<const f32>(samples.depth.data(), samples.depth.size());
    inputs.velocity = Span<const Vec2>(samples.velocity.data(), samples.velocity.size());
    // Inside the square every sample is the square; outside it, the background.
    const ResampledTexel inside = resample_reference_at(inputs, run.constants(), 46, 36);
    CY_CHECK_NEAR(inside.depth, device_depth(5.0F), 1e-6F);
    CY_CHECK_NEAR(inside.velocity.x, -kSquareSpeed / static_cast<f32>(kOutput.width), 1e-6F);
    const ResampledTexel outside = resample_reference_at(inputs, run.constants(), 10, 10);
    CY_CHECK_NEAR(outside.depth, device_depth(10.0F), 1e-6F);
    CY_CHECK_EQ(outside.velocity.x, 0.0F);
}
