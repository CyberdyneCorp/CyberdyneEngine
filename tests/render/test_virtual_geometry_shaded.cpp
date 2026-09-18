// The SHADED virtual-geometry frame, photographed and compared against a committed reference.
// M11.c, and the criterion `m11c:virtual-geometry-image`.
//
// ================================================================================================
// WHAT WAS WRONG BEFORE THIS FILE EXISTED
// ================================================================================================
//
// `virtual-geometry` and `virtual-shadows` had a picture — `docs/design/images/virtual-geometry-
// shaded.png` — and NOTHING ASSERTED IT. It was written by `just capture-virtual-geometry`, a
// recipe a person runs; no test re-photographed it and no test compared it, so the frame could have
// stopped being a frame between two milestones and the evidence would have gone on sitting in the
// documentation looking exactly as convincing. Its caption said what it actually was: "Resolved
// world normals under one light" — a debug view, not a shading result.
//
// The suites `render.virtual_geometry_gpu` already had are traversal, residency, occlusion and
// visibility-buffer cases: every one of them compares the device against the CPU reference BUFFER
// FOR BUFFER, which is the right claim and is not a picture. This is the picture.
//
// ================================================================================================
// WHAT IS PHOTOGRAPHED IS THE SAMPLE, NOT A COPY OF IT
// ================================================================================================
//
// The frame is `samples/07-fidelity`'s, reached through `cy::sample-fidelity-frame` — the library
// half of the M7 artefact — at shot parameter 0, the same frame the documentation publishes. The
// scene is the artefact's own: 4,478,208 source triangles from 116,928 distinct ones, cooked
// through `virtual-geometry`'s real builder and drawn through the cluster traversal and the
// visibility buffer on a real device. The viewport is smaller than the documentation's, and that is
// the only difference: a committed reference is an uncompressed PNG and 1280x720 of one costs
// 3.5 MB.
//
// ================================================================================================
// THE TOLERANCE, AND WHERE THE NUMBER CAME FROM
// ================================================================================================
//
// `golden.h`'s, unchanged and not re-derived here: a texel differs when any channel differs by more
// than two 8-bit steps, and a texel may differ AT ALL only where the reference itself has a
// high-contrast four-neighbour. `differing_off_edge` must be zero.
//
// IT WAS MEASURED RATHER THAN CHOSEN. This frame was rendered THREE times on this machine, in
// three separate processes — the run that wrote the reference and two that compared against it —
// and all three are BIT-IDENTICAL: 0 of 57,600 texels differ, maximum channel delta 0. The
// reference's own derived edge budget is 8,521 texels and NONE of them was needed. That is the
// property `render.frames` already asserts about the M3 artefact for the same reason, and it is
// why the tolerance above is headroom for another implementation rather than for this one. A
// tolerance chosen as a round number, without measuring what actually moves, is a tolerance that
// hides the regression it was sized for.
//
// ================================================================================================
// THE PICTURE IS OF TWO ROWS, AND THE SECOND CASE IS WHY THE FIRST IS EVIDENCE ABOUT BOTH
// ================================================================================================
//
// The sun in this frame is shadowed through `src/rendering/shadows/` — clipmap levels snapped to
// page boundaries, a page per receiver picked from its projected texel density, a physical page
// cache that allocates and evicts, pages rasterised once from the scene's level-0 clusters, and
// every lookup walked through the fallback chain. `shade.h` carries that argument in full.
//
// ================================================================================================
// PROVED RED BY BREAKING THE SUBJECT, TWICE — ONCE PER ROW
// ================================================================================================
//
// A golden-image case that has only ever been seen green is a golden-image case nobody has shown
// can fail, and breaking the COMPARISON or the reference's PATH proves only that a file is read.
// Both mutations below break the thing the picture is OF. Measured against `build/m11c-final` on
// an RTX 5060; each was restored afterwards and the suite re-run green.
//
//   `virtual-geometry` — delete `out.clusters[index].child_count = out.groups[from].member_count;`
//   from `Finaliser::emit_hierarchy()` in `src/rendering/virtual_geometry/src/build.cpp`. Every
//   cooked cluster then reports zero children, `clusterTooCoarse(...) && cluster.childCount > 0`
//   in `vg_traversal.slang` is never true, and THE HIERARCHY STOPS DESCENDING: the device draws
//   each instance's coarsest cluster and nothing under it. The cook still succeeds and validation
//   is still clean. Clusters decoded 3,324 -> 246, coverage 57,600 -> 55,322, and 46,257 of 57,600
//   texels move — 80.31% of the frame, 38,878 of them off any high-contrast edge, worst channel
//   delta 188 of 255. Three assertions of the first case go red.
//
//   `virtual-shadows` — delete `entry.state = PageState::Resident;` from
//   `ShadowPageCache::record_render` in `src/rendering/shadows/src/cache.cpp`. A page is then
//   rasterised and never MARKED resident, so all 27,095 sun-facing lookups fall through the
//   fallback chain: shadowed pixels 24,649 -> 0, and 24,578 of 57,600 texels move — 42.67%,
//   20,089 of them off-edge, worst channel delta 163. Four assertions go red, and the first of
//   them is `shade_report.shadowed > 0`, which is the counter that exists so a shadow cannot
//   vanish quietly.
//
// This is the criterion's own declared mutation — `tools/roadmap/milestones/m11c.toml`,
// `[criterion.falsifies]` under `virtual-geometry-image` — so the prover re-runs the first of the
// two rather than trusting this comment.
//
// ================================================================================================
// THE PICTURE IS OF TWO ROWS, AND THE SECOND CASE IS WHY THE FIRST IS EVIDENCE ABOUT BOTH (CONT.)
// ================================================================================================
//
// So the second case starves the page cache to a single slot. Every receiver is then refused a
// page, the chain falls all the way to `Unshadowed`, and the frame CHANGES — measurably, and the
// case says by how much. Without it, a reference that had quietly stopped sampling the shadow map
// would keep passing the first case forever, which is exactly the argument `render.golden`'s third
// case makes about the first-light shadow and the reason it carries a second reference.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>

#include "frame.h"
#include "golden.h"
#include "scene.h"
#include "shade.h"

#include <cstdio>
#include <cstdlib>

namespace {

using cy::sample::fidelity::Capture;
using cy::sample::fidelity::FrameOptions;
using cy::sample::fidelity::FrameReport;
using cy::sample::fidelity::Scene;
using cy::sample::fidelity::SceneOptions;
using cy::sample::fidelity::ShadedFrame;
using cy::sample::fidelity::ShadeOptions;
using cy::sample::fidelity::ShadeReport;

/// The golden viewport. Small on purpose, for the reason `test_golden_frame.cpp` gives: the
/// reference is a committed binary and this encoder stores rather than deflates, so 320x180 costs
/// about 173 KiB. It is still eight times the M3 reference's area, which is what the hall, the
/// forty columns, the twenty-four statues and the shadow they cast need to be separable in it.
constexpr cy::u32 kWidth = 320;
constexpr cy::u32 kHeight = 180;

/// Phase 0 of the shot — inside the hall — stated rather than implied. It is the frame
/// `docs/design/virtual-geometry.md` publishes and the one a reader can reproduce from the scene
/// alone.
constexpr cy::f32 kShot = 0.0F;

const char* reference_path() noexcept {
    static char storage[1024];
    std::snprintf(storage, sizeof(storage), "%s/references/virtual_geometry_shaded.png",
                  CY_RENDER_TEST_DIR);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Cook the set, render one frame of it on the device, and shade the result.
///
/// Returns false when there is no device, having said so — the same loud skip every suite in this
/// directory takes, and the reason `m11c:virtual-geometry-image` runs the suite by name rather than
/// through a label selector that would call an empty run a pass.
bool render_shaded(cy::Allocator& allocator, const ShadeOptions& shade_options,
                   cy::render_test::Image& out, ShadeReport& shade_report) {
    Scene scene(allocator);
    const cy::Status built = cy::sample::fidelity::build_scene(SceneOptions{}, scene);
    CY_CHECK(built.has_value());
    if (!built.has_value()) {
        return false;
    }

    FrameOptions options;
    options.width = kWidth;
    options.height = kHeight;
    options.frames = 1;
    options.warmup_frames = 0;
    options.threshold_pixels = 1.0F;
    options.capture_shot = kShot;

    FrameReport report(allocator);
    Capture capture(allocator);
    const cy::Status ran = cy::sample::fidelity::render_frames(scene, options, report, &capture);
    CY_CHECK(ran.has_value());
    if (!ran.has_value()) {
        return false;
    }
    if (!report.device) {
        // `render_frames` answers the null backend when no Vulkan device can be created, exactly as
        // `device.h` describes. A skip is not a pass and the criterion's own guard greps for this
        // line, so it is printed in the shape every suite here prints it.
        std::fprintf(stderr, "no Vulkan device on this machine; the backend selected was '%s'\n",
                     report.backend);
        return false;
    }
    CY_CHECK(report.validation_errors == 0);

    ShadedFrame shaded(allocator);
    const cy::Status lit =
        cy::sample::fidelity::shade_frame(scene, capture, shade_options, shaded, shade_report);
    CY_CHECK(lit.has_value());
    if (!lit.has_value()) {
        return false;
    }
    const cy::Status adopted =
        cy::render_test::adopt(out, shaded.texels.span(), shaded.width, shaded.height);
    CY_CHECK(adopted.has_value());
    return adopted.has_value();
}

}  // namespace

CY_TEST_CASE("the shaded virtual-geometry frame matches its committed reference") {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);

    cy::render_test::Image rendered(allocator);
    ShadeReport shade_report;
    if (!render_shaded(allocator, ShadeOptions{}, rendered, shade_report)) {
        return;
    }

    // THE PICTURE HAS TO BE OF SOMETHING, and these are the ways it could be of nothing while still
    // matching a reference regenerated beside the defect. A frame with no covered pixels, a frame
    // whose surfaces could not be reconstructed, a shadow map with no pages and no texels in them,
    // or a frame that is entirely lit or entirely shadowed are each a picture that would compare
    // equal to itself forever.
    std::fprintf(stderr,
                 "shaded: %u covered, %u sunlit, %u shadowed, %u facing away, %u clusters; "
                 "shadow: %u pages rendered of %u requested, %u starved, %u texels, %u of %u "
                 "lookups substituted\n",
                 shade_report.covered, shade_report.lit_by_sun, shade_report.shadowed,
                 shade_report.facing_away, shade_report.clusters_decoded,
                 shade_report.pages_rendered, shade_report.pages_requested,
                 shade_report.pages_starved, shade_report.shadow_texels_written,
                 shade_report.substitutions.substituted(), shade_report.substitutions.total());
    CY_CHECK(shade_report.covered > (kWidth * kHeight) / 4U);
    CY_CHECK(shade_report.unreconstructed == 0);
    CY_CHECK(shade_report.clusters_decoded > 0);
    CY_CHECK(shade_report.pages_rendered > 0);
    CY_CHECK(shade_report.shadow_texels_written > 0);
    CY_CHECK(shade_report.shadowed > 0);
    CY_CHECK(shade_report.lit_by_sun > 0);

    if (updating_references()) {
        const cy::Status written = cy::render_test::write_png(reference_path(), rendered);
        CY_CHECK(written.has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     reference_path());
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }

    cy::render_test::Image reference(allocator);
    const cy::Status read = cy::render_test::read_png(reference_path(), reference);
    CY_REQUIRE(read.has_value());

    const cy::render_test::Comparison comparison = cy::render_test::compare(reference, rendered);
    CY_CHECK(comparison.comparable);
    std::fprintf(stderr,
                 "golden: %u differing (%u off edge) of %u, edge budget %u, worst delta %u at "
                 "(%u, %u)\n",
                 comparison.differing, comparison.differing_off_edge, kWidth * kHeight,
                 comparison.edge_texels, comparison.max_channel_delta, comparison.worst_x,
                 comparison.worst_y);
    if (comparison.differing != 0) {
        const cy::Status wrote = cy::render_test::write_difference(
            "virtual-geometry-shaded-difference.png", reference, rendered);
        CY_CHECK(wrote.has_value());
    }
    // A difference away from a high-contrast edge is a shading difference, not a rounding one.
    CY_CHECK(comparison.differing_off_edge == 0);
    CY_CHECK(comparison.differing <= comparison.edge_texels);
    // On the machine the reference came from the frame is bit-identical, and that is the stronger
    // claim. It is checked separately so that a failure says which of the two it was.
    CY_CHECK(comparison.differing == 0);
}

CY_TEST_CASE("starving the shadow page cache changes the picture") {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);

    // ONE PHYSICAL PAGE for the whole frame. `ShadowPageCache::request` seats the first receiver's
    // page and refuses every page after it — a slot used this frame is not a candidate for eviction
    // — so the fallback chain runs out and the sun arrives unshadowed almost everywhere.
    ShadeOptions starved;
    starved.shadow_slots = 1;

    cy::render_test::Image rendered(allocator);
    ShadeReport report;
    if (!render_shaded(allocator, starved, rendered, report)) {
        return;
    }
    const cy::u32 unshadowed =
        report.substitutions
            .counts[static_cast<cy::usize>(cy::rendering::ShadowSubstitution::Unshadowed)];
    std::fprintf(stderr,
                 "starved: %u pages starved of %u requested, %u of %u lookups unshadowed, "
                 "%u shadowed against %u sunlit\n",
                 report.pages_starved, report.pages_requested, unshadowed,
                 report.substitutions.total(), report.shadowed, report.lit_by_sun);
    CY_CHECK(report.pages_starved > 0);
    // MEASURED ON THIS TREE, not guessed at. With 1024 slots the frame is 24,649 shadowed pixels
    // against 2,446 sunlit ones; with one slot it is 4,585 against 22,510, because 22,507 of the
    // 27,095 sun-facing lookups fall all the way through the chain to `Unshadowed`. The claim below
    // is the collapse itself — a majority of lookups unshadowed, and the shadow no longer the thing
    // most of the sun-facing surface is in — stated as a floor well inside the measurement so that
    // a driver whose page eviction lands differently does not fail for that.
    CY_CHECK(unshadowed * 2U > report.substitutions.total());
    CY_CHECK(report.shadowed * 2U < report.lit_by_sun);

    cy::render_test::Image reference(allocator);
    const cy::Status read = cy::render_test::read_png(reference_path(), reference);
    CY_REQUIRE(read.has_value());
    const cy::render_test::Comparison comparison = cy::render_test::compare(reference, rendered);
    CY_CHECK(comparison.comparable);
    std::fprintf(stderr, "starved: %u of %u texels differ from the committed reference\n",
                 comparison.differing, kWidth * kHeight);
    // A tenth of the frame is a floor, not a measurement: what is actually measured is printed
    // above. The claim is only that losing the shadow is not a difference a tolerance could absorb.
    CY_CHECK(comparison.differing > (kWidth * kHeight) / 10U);
}
