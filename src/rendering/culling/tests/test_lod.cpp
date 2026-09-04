// Screen coverage, LOD selection with hysteresis, visibility ranges and HLOD. Task 4.4.3.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/culling/lod.h>

namespace {

using cy::rendering::FadeMode;
using cy::rendering::HlodResolution;
using cy::rendering::kInvalidLod;
using cy::rendering::kInvalidVisibilityParent;
using cy::rendering::LodSelection;
using cy::rendering::LodSettings;
using cy::rendering::VisibilityRange;

/// A three-level chain whose thresholds descend, as `render::MeshLod` requires.
constexpr cy::render::MeshLod kChain[3] = {
    {0, 1, 0.5F, 1000},
    {1, 1, 0.2F, 400},
    {2, 1, 0.0F, 100},
};

cy::Span<const cy::render::MeshLod> chain() noexcept {
    return {kChain, 3};
}

}  // namespace

CY_TEST_CASE("coverage falls with distance and is independent of resolution") {
    // Coverage is a fraction of the viewport HEIGHT, so it depends on the field of view and the
    // geometry and on nothing else — which is why a resolution change does not re-author LODs.
    const cy::f32 near_coverage = cy::rendering::screen_coverage(1.0F, 2.0F, 1.0471975512F);
    const cy::f32 far_coverage = cy::rendering::screen_coverage(1.0F, 20.0F, 1.0471975512F);
    CY_CHECK_GT(near_coverage, far_coverage);
    CY_CHECK_NEAR(near_coverage / far_coverage, 10.0F, 1e-3F);

    // An object the camera is inside covers the screen. Returning 0 there would drop it to its
    // coarsest level exactly when it fills the frame.
    CY_CHECK_EQ(cy::rendering::screen_coverage(1.0F, 0.0F, 1.0471975512F), 1.0F);
}

CY_TEST_CASE(
    "selection walks the chain, and a mesh below every threshold gets the coarsest level") {
    LodSettings settings;
    settings.hysteresis = 0.0F;

    CY_CHECK_EQ(cy::rendering::select_lod(chain(), 0.9F, 0.0F, settings, kInvalidLod).level, 0U);
    CY_CHECK_EQ(cy::rendering::select_lod(chain(), 0.3F, 0.0F, settings, kInvalidLod).level, 1U);
    CY_CHECK_EQ(cy::rendering::select_lod(chain(), 0.01F, 0.0F, settings, kInvalidLod).level, 2U);
}

CY_TEST_CASE("hysteresis stops an instance oscillating on a threshold") {
    LodSettings settings;
    settings.hysteresis = 0.25F;

    // Coverage just below level 0's threshold. With no history the plain threshold applies and the
    // instance coarsens; already at level 0 it stays there, which is the whole point of the band.
    const cy::f32 coverage = 0.45F;
    CY_CHECK_EQ(cy::rendering::select_lod(chain(), coverage, 0.0F, settings, kInvalidLod).level,
                1U);
    CY_CHECK_EQ(cy::rendering::select_lod(chain(), coverage, 0.0F, settings, 0U).level, 0U);

    // Far enough below the reduced threshold and it coarsens even from level 0 — the band is a
    // delay, not a latch.
    CY_CHECK_EQ(cy::rendering::select_lod(chain(), 0.3F, 0.0F, settings, 0U).level, 1U);
}

CY_TEST_CASE("bias scales coverage, so a shadow view selects coarser levels") {
    LodSettings settings;
    settings.hysteresis = 0.0F;
    const cy::f32 coverage = 0.6F;
    CY_CHECK_EQ(cy::rendering::select_lod(chain(), coverage, 0.0F, settings, kInvalidLod).level,
                0U);

    // A shadow view's own negative bias: "shadow and reflection views SHALL use their own bias so
    // lower LODs are used where detail is not visible".
    settings.view_bias = -1.0F;  // one stop of coverage
    CY_CHECK_EQ(cy::rendering::select_lod(chain(), coverage, 0.0F, settings, kInvalidLod).level,
                1U);
}

CY_TEST_CASE("the cross-fade band names both levels") {
    LodSettings settings;
    settings.hysteresis = 0.0F;
    settings.cross_fade_band = 0.2F;

    // Just inside the band above level 0's threshold: both levels are drawn with complementary
    // dither masks, and temporal antialiasing resolves the blend.
    const LodSelection selection =
        cy::rendering::select_lod(chain(), 0.55F, 0.0F, settings, kInvalidLod);
    CY_CHECK_EQ(selection.level, 0U);
    CY_CHECK_EQ(selection.fade_to, 1U);
    CY_CHECK_GT(selection.fade, 0.0F);
    CY_CHECK_LE(selection.fade, 1.0F);

    // Well inside level 0 there is no transition in flight.
    const LodSelection settled =
        cy::rendering::select_lod(chain(), 5.0F, 0.0F, settings, kInvalidLod);
    CY_CHECK_EQ(settled.fade_to, kInvalidLod);
    CY_CHECK_EQ(settled.fade, 0.0F);
}

CY_TEST_CASE("a visibility range fades in and out over its margin") {
    VisibilityRange range;
    range.begin = 10.0F;
    range.end = 100.0F;
    range.fade_margin = 5.0F;

    CY_CHECK_EQ(cy::rendering::visibility_range_alpha(range, 50.0F), 1.0F);
    CY_CHECK_EQ(cy::rendering::visibility_range_alpha(range, 0.0F), 0.0F);
    CY_CHECK_NEAR(cy::rendering::visibility_range_alpha(range, 7.5F), 0.5F, 1e-4F);
    CY_CHECK_NEAR(cy::rendering::visibility_range_alpha(range, 102.5F), 0.5F, 1e-4F);

    // A zeroed range is "always visible", which is what makes it the default nobody has to set.
    CY_CHECK_EQ(cy::rendering::visibility_range_alpha(VisibilityRange{}, 1e6F), 1.0F);
}

CY_TEST_CASE("an HLOD proxy replaces its children, and nesting resolves to one level per branch") {
    // 0 is the coarsest proxy, 1 its child proxy, 2 a leaf under 1.
    VisibilityRange ranges[3];
    ranges[0].begin = 100.0F;
    ranges[0].parent = kInvalidVisibilityParent;
    ranges[1].begin = 50.0F;
    ranges[1].end = 100.0F;
    ranges[1].parent = 0;
    ranges[2].end = 50.0F;
    ranges[2].parent = 1;

    HlodResolution out[3];
    cy::f32 distances[3] = {150.0F, 150.0F, 150.0F};
    CY_REQUIRE(cy::rendering::resolve_hlod(cy::Span<const VisibilityRange>(ranges, 3),
                                           cy::Span<const cy::f32>(distances, 3),
                                           cy::Span<HlodResolution>(out, 3))
                   .has_value());
    // Far away: only the coarsest proxy draws.
    CY_CHECK(out[0].visible);
    CY_CHECK_FALSE(out[1].visible);
    CY_CHECK_FALSE(out[2].visible);

    // Close in: only the leaf.
    distances[0] = 10.0F;
    distances[1] = 10.0F;
    distances[2] = 10.0F;
    CY_REQUIRE(cy::rendering::resolve_hlod(cy::Span<const VisibilityRange>(ranges, 3),
                                           cy::Span<const cy::f32>(distances, 3),
                                           cy::Span<HlodResolution>(out, 3))
                   .has_value());
    CY_CHECK_FALSE(out[0].visible);
    CY_CHECK_FALSE(out[1].visible);
    CY_CHECK(out[2].visible);
}

CY_TEST_CASE("a dependents fade draws parent and child together during the swap") {
    VisibilityRange ranges[2];
    ranges[0].begin = 100.0F;
    ranges[0].fade_margin = 20.0F;
    ranges[0].mode = FadeMode::Dependents;
    ranges[0].parent = kInvalidVisibilityParent;
    ranges[1].end = 0.0F;  // unbounded: the child is otherwise always visible
    ranges[1].parent = 0;

    // Inside the parent's fade-in margin: both are drawn, with complementary alphas.
    HlodResolution out[2];
    const cy::f32 distances[2] = {90.0F, 90.0F};
    CY_REQUIRE(cy::rendering::resolve_hlod(cy::Span<const VisibilityRange>(ranges, 2),
                                           cy::Span<const cy::f32>(distances, 2),
                                           cy::Span<HlodResolution>(out, 2))
                   .has_value());
    CY_CHECK(out[0].visible);
    CY_CHECK(out[1].visible);
    CY_CHECK_NEAR(out[0].alpha + out[1].alpha, 1.0F, 1e-4F);
}

CY_TEST_CASE("a cycle in the parent links is refused rather than hanging the frame") {
    // Both are outside their own ranges, so neither is visible and the walk to the root never finds
    // one to stop at — which is exactly the shape that would spin forever without the step guard.
    VisibilityRange ranges[2];
    ranges[0].begin = 100.0F;
    ranges[0].parent = 1;
    ranges[1].begin = 100.0F;
    ranges[1].parent = 0;
    HlodResolution out[2];
    const cy::f32 distances[2] = {1.0F, 1.0F};
    CY_CHECK_FALSE(cy::rendering::resolve_hlod(cy::Span<const VisibilityRange>(ranges, 2),
                                               cy::Span<const cy::f32>(distances, 2),
                                               cy::Span<HlodResolution>(out, 2))
                       .has_value());
}
