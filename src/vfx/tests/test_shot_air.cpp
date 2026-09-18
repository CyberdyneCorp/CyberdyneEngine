// THE ARTEFACT'S AIR, PHOTOGRAPHED AND COMPARED. M11.c task 6.3, and `m11c:vfx-in-the-shot`.
//
// ================================================================================================
// WHY THIS CASE EXISTS, AND WHAT WAS THERE BEFORE IT
// ================================================================================================
//
// The rule since M8.c is that anything with a visible result gets an image, and `vfx-system`'s
// images were `just`-driven captures: `docs/design/images/vfx-simulation.png` and the budget pair,
// written by a recipe, re-photographed by nothing and compared against nothing. The beauty shot was
// the same shape — `just capture-beauty-shot` produces a picture that no suite renders again.
//
// So this case is the missing half: it RENDERS the artefact's air, through the artefact's camera,
// at the artefact's grade, and compares the result against a COMMITTED REFERENCE within a tolerance
// that was measured rather than chosen. When the thing the picture is of stops working, it fails.
//
// ================================================================================================
// WHAT IS IN THE PICTURE AND WHAT IS NOT
// ================================================================================================
//
// IN IT: `samples/12-beauty/embers.cpp`'s cooked system — the same authored graphs, the same
// precision selection, the same three emitters at the same world positions — simulated by
// `SimulationWorld` for `kEmberWarmup` seconds, published by `publish_sprites` against the shot's
// own camera position, uploaded into `ParticleRenderer`'s ring and drawn by its own extension
// inside `FrameAssembly`'s TRANSPARENT stage, through the same post chain and the same exposure the
// artefact is graded at.
//
// NOT IN IT: the colonnade. The shot's geometry is shaded by material programs `slangc` compiles AT
// CAPTURE TIME out of the committed `.cygraph` files — `samples/12-beauty/CMakeLists.txt` says why
// no SPIR-V is embedded — and a suite cannot depend on a shader compiler being on the machine. So
// what is committed here is the artefact's TRANSPARENT LAYER over the frame's own clear, which is
// exactly the contribution `vfx-system` makes to the published still and exactly the thing that
// disappears when the row breaks.
//
// The other half of the claim — that this air is WHERE THE CAMERA IS LOOKING — is
// `integration.vfx`'s case of the same name, which projects every published record through the
// artefact's camera on a machine with no GPU at all.
//
// ================================================================================================
// THE TOLERANCE IS A MEASUREMENT
// ================================================================================================
//
// It was not chosen and it was not rounded. The case renders THE SAME FRAME TWICE in one process
// and asserts the two are byte-identical, which is what this driver actually does — measured: 0
// texels of 129 600 differ, maximum channel delta 0. The comparison against the committed reference
// then uses `golden.h`'s metric, whose two texels of channel tolerance are one quantisation step of
// an 8-bit target plus one step of headroom, and asserts the stronger claim as well: on the machine
// the reference came from, `differing` is 0.
//
// A round number would have hidden a regression. Three of these motes are two pixels across; a
// tolerance of "a few percent of texels" would let the whole near emitter vanish and still pass.
//
// ================================================================================================
// AND THE NEGATIVE CONTROL IS IN THE CASE
// ================================================================================================
//
// `m11c:vfx-in-the-shot` asks for a difference that is NAMED: "a frame with an effect and the same
// frame without it must differ measurably, and the assertion must name how much". So the case
// renders the identical frame with the ring uploaded EMPTY — same passes, same clear, same resolve,
// same exposure, same extension attached — and asserts the measured difference.
//
// MEASURED, on an RTX 5060 at 480x270: 4 713 of 129 600 texels differ, which is 3.64% of the
// frame; mean |delta| is 0.753 of 255 over the whole frame and 20.7 over the texels that differ;
// the worst channel moves by 255 — a fresh mote's core clips, which is what `embers.h` picked its
// radiance to do. Every one of those is printed each run and three of them are floored, at roughly
// half the measurement — because the repeat-render difference above is exactly zero, so there is
// no noise to clear and the floors exist to catch an effect that got WEAKER rather than one that
// moved.

#include "embers.h"
#include "vfx_scene.h"

#include "device.h"
#include "golden.h"

#include <cy/core/math/projection.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

using namespace cy;
using namespace cy::vfx_test;
using cy::render_test::DeviceFixture;
using cy::sample::beauty::cook_embers;
using cy::sample::beauty::kEmberEmitters;
using cy::sample::beauty::kEmberStep;
using cy::sample::beauty::kEmberWarmup;
using cy::sample::beauty::kShotExposureStops;
using cy::sample::beauty::kShotEye;
using cy::sample::beauty::kShotFovDegrees;
using cy::sample::beauty::kShotTarget;

namespace {

/// The artefact is 1920x1080 and this reference is 480x270. The ASPECT is what decides the framing
/// and it is the same one, so the air sits where it sits in the still.
inline constexpr f32 kShotAspect = static_cast<f32>(kSceneWidth) / static_cast<f32>(kSceneHeight);

[[nodiscard]] f32 shot_fov_y() noexcept {
    constexpr f32 kHalfDegreesToRadians = std::numbers::pi_v<f32> / 360.0F;
    return 2.0F * std::atan(std::tan(kShotFovDegrees * kHalfDegreesToRadians) / kShotAspect);
}

const char* reference_path() noexcept {
    static char storage[1024];
    std::snprintf(storage, sizeof(storage), "%s/references/beauty_shot_air.png",
                  CY_RENDER_TEST_DIR);
    return storage;
}

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// What two images differ by, in the two numbers that matter for a field of small bright sprites.
struct Difference {
    u32 differing = 0;
    f64 mean_delta = 0.0;
    f64 mean_delta_where_differing = 0.0;
    u32 max_delta = 0;
};

[[nodiscard]] Difference difference(Span<const u32> a, Span<const u32> b) noexcept {
    Difference out;
    if (a.size() != b.size() || a.empty()) {
        return out;
    }
    f64 total = 0.0;
    f64 total_differing = 0.0;
    for (usize index = 0; index < a.size(); ++index) {
        u32 worst = 0;
        f64 sum = 0.0;
        for (u32 channel = 0; channel < 3U; ++channel) {
            const auto left = static_cast<i32>((a[index] >> (channel * 8U)) & 0xFFU);
            const auto right = static_cast<i32>((b[index] >> (channel * 8U)) & 0xFFU);
            const auto delta = static_cast<u32>(left > right ? left - right : right - left);
            worst = delta > worst ? delta : worst;
            sum += static_cast<f64>(delta);
        }
        total += sum / 3.0;
        if (worst != 0) {
            ++out.differing;
            total_differing += sum / 3.0;
        }
        out.max_delta = worst > out.max_delta ? worst : out.max_delta;
    }
    out.mean_delta = total / static_cast<f64>(a.size());
    out.mean_delta_where_differing =
        out.differing == 0 ? 0.0 : total_differing / static_cast<f64>(out.differing);
    return out;
}

/// Copy a frame out of the scene so a second render can be compared against it.
[[nodiscard]] Status keep(Allocator& allocator, Span<const u32> texels, Array<u32>& out) noexcept {
    (void)allocator;
    if (Status sized = out.resize(texels.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < texels.size(); ++index) {
        out[index] = texels[index];
    }
    return ok();
}

}  // namespace

CY_TEST_CASE("particles are in the assembled frame") {
    DeviceFixture fixture("vulkan", "cy_test_render_vfx");
    if (!fixture.is(rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    // --- The artefact's own effect, cooked by the shipping compiler --------------------------
    graph::DiagnosticSink sink(allocator);
    vfx::CompileReport cook(allocator);
    Expected<vfx::CompiledSystem, Error> system = cook_embers(allocator, sink, cook);
    CY_REQUIRE(system.has_value());

    Array<Vec3> spawns(allocator);
    for (const auto& emitter : kEmberEmitters) {
        CY_REQUIRE(spawns.push_back(emitter.position).has_value());
    }

    SceneOptions options;
    options.system = &system.value();
    options.spawns = spawns.span();
    options.eye = kShotEye;
    options.target = kShotTarget;
    options.fov_y_radians = shot_fov_y();
    options.exposure_stops = kShotExposureStops;

    VfxScene scene(allocator);
    CY_REQUIRE(scene.build(fixture.device(), options).has_value());
    scene.set_read_back(true);

    // --- Settle the field, exactly as `Stage::stage_shot` settles the artefact's ----------------
    // `lround` rather than `+ 0.5F` and a cast: the two agree here — the quotient is 360
    // exactly — and clang-tidy is right that they do not agree in general.
    const auto steps = static_cast<u32>(std::lround(kEmberWarmup / kEmberStep));
    for (u32 step = 0; step < steps; ++step) {
        CY_REQUIRE(scene.simulate(kEmberStep).has_value());
    }
    std::fprintf(stderr, "the shot's air: %u mote(s) published, %u dropped\n",
                 scene.published().particles, scene.published().dropped);
    CY_CHECK_GT(scene.published().particles, 900U);
    CY_CHECK_EQ(scene.published().dropped, 0U);

    // --- The frame --------------------------------------------------------------------------
    assembly::AssemblyReport report{};
    CY_REQUIRE(scene.render(report).has_value());
    // ONE DRAW FOR THE WHOLE FIELD, and the renderer's own count of what it drew. A picture that
    // matched its reference while this was zero would be a picture of a coincidence.
    CY_CHECK_EQ(scene.particle_report().draws, 1U);
    CY_CHECK_EQ(scene.particle_report().particles, scene.published().particles);
    CY_CHECK_EQ(scene.particle_report().dropped, 0U);

    Array<u32> with_air(allocator);
    CY_REQUIRE(keep(allocator, scene.pixels(), with_air).has_value());

    // --- THE TOLERANCE, MEASURED: the same frame again ----------------------------------------
    //
    // No simulation step between the two, so this is the device rendering identical input twice.
    // Whatever moves here is the floor under any comparison against a committed file, and on this
    // driver nothing does.
    CY_REQUIRE(scene.render(report).has_value());
    const Difference repeat = difference(with_air.span(), scene.pixels());
    std::fprintf(stderr,
                 "the shot's air, rendered twice: %u of %u texel(s) differ, mean |delta| %.4f, "
                 "max channel delta %u\n",
                 repeat.differing, static_cast<u32>(with_air.size()), repeat.mean_delta,
                 repeat.max_delta);
    CY_CHECK_EQ(repeat.differing, 0U);

    // --- THE NEGATIVE CONTROL: the same frame with nothing in the ring ------------------------
    scene.set_draw_particles(false);
    CY_REQUIRE(scene.render(report).has_value());
    CY_CHECK_EQ(scene.particle_report().particles, 0U);
    const Difference control = difference(with_air.span(), scene.pixels());
    std::fprintf(stderr,
                 "the shot's air against the same frame with none: %u of %u texel(s) differ "
                 "(%.2f%%), mean |delta| %.3f/255 over the frame and %.1f/255 where they differ, "
                 "max %u\n",
                 control.differing, static_cast<u32>(with_air.size()),
                 100.0 * static_cast<f64>(control.differing) / static_cast<f64>(with_air.size()),
                 control.mean_delta, control.mean_delta_where_differing, control.max_delta);
    // HOW MUCH, NAMED. Measured: 4 713 texels of 129 600 (3.64%), mean |delta| 20.7/255 where they
    // differ, worst channel 255. The floors are about half of each. Three of them rather than one,
    // because each fails on a different way for the air to stop being in the picture: a field that
    // shrank to a handful of motes drops the count, a field drawn at the wrong exposure or with the
    // blend inverted drops the mean, and a field whose sprites lost their cores drops the maximum.
    CY_CHECK_GT(control.differing, with_air.size() / 64U);
    CY_CHECK_GT(control.mean_delta_where_differing, 10.0);
    CY_CHECK_GT(control.max_delta, 127U);
    scene.set_draw_particles(true);

    // --- THE COMMITTED REFERENCE --------------------------------------------------------------
    render_test::Image rendered(allocator);
    CY_REQUIRE(
        render_test::adopt(rendered, with_air.span(), kSceneWidth, kSceneHeight).has_value());
    const char* path = reference_path();
    if (updating_references()) {
        CY_CHECK(render_test::write_png(path, rendered).has_value());
        std::fprintf(stderr,
                     "CY_RENDER_UPDATE_GOLDEN: wrote %s. Look at it, then commit it — this run "
                     "fails on purpose so that a regenerating run can never be a passing one.\n",
                     path);
        CY_CHECK_FALSE(updating_references());
        return;
    }

    render_test::Image reference(allocator);
    const Status read = render_test::read_png(path, reference);
    if (!read) {
        std::fprintf(stderr, "the shot's air: %s: %s\n", path, read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    CY_CHECK(comparison.comparable);
    if (comparison.differing != 0) {
        (void)render_test::write_difference("beauty-shot-air-difference.png", reference, rendered);
        std::fprintf(stderr,
                     "the shot's air differs from its reference — %u texel(s) over tolerance (%u "
                     "of them away from any high-contrast edge), worst channel delta %u at "
                     "(%u, %u). The difference is at beauty-shot-air-difference.png.\n",
                     comparison.differing, comparison.differing_off_edge,
                     comparison.max_channel_delta, comparison.worst_x, comparison.worst_y);
    }
    CY_CHECK_EQ(comparison.differing_off_edge, 0U);
    CY_CHECK_LE(comparison.differing, comparison.edge_texels);
    // The stronger claim, which holds on the machine the reference came from and is the one a
    // regression breaks first.
    CY_CHECK_EQ(comparison.differing, 0U);

    // `rhi-and-render-graph`: a frame that renders but trips validation is not a frame that works.
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
