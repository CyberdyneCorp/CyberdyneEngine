// Reversed-Z, asserted from the device. Task 5.1, design.md §3.
//
// "The depth buffer is `[0,1]`, cleared to **0**, compared **GreaterEqual**, and a perspective
// matrix built by the engine maps near to 1 and far to 0 *as sampled back from the device*. A
// golden image is not sufficient evidence — a scene can look right with an inverted comparison
// until something intersects."
//
// So every assertion below is on a number copied off the device: the depth image's own texels,
// after a real draw with a real pipeline. M1 made these conventions arithmetic; this is where they
// meet a depth buffer.

#include "probe.h"

#include <cy/core/math/projection.h>

namespace {

using cy::render_test::ProbeFixture;
using cy::render_test::ProbeImage;
using cy::render_test::ProbeTriangle;

/// A triangle that covers the whole viewport at one camera-relative depth. The camera looks down
/// its local −Z, so a point in front of it has a NEGATIVE z — which is why every distance below is
/// negated exactly once, here.
ProbeTriangle covering(cy::f32 distance, cy::f32 red) noexcept {
    // Oversized on purpose: the clip-space triangle must cover every pixel, and a triangle exactly
    // the size of the frustum leaves the corners to rounding.
    const cy::f32 extent = distance * 4.0F;
    ProbeTriangle triangle;
    triangle.vertices[0] = cy::Vec3{-extent, -extent, -distance};
    triangle.vertices[1] = cy::Vec3{extent * 3.0F, -extent, -distance};
    triangle.vertices[2] = cy::Vec3{-extent, extent * 3.0F, -distance};
    triangle.color[0] = red;
    triangle.color[1] = 0.0F;
    triangle.color[2] = 0.0F;
    triangle.color[3] = 1.0F;
    return triangle;
}

constexpr cy::f32 kNear = 0.5F;
constexpr cy::f32 kFar = 100.0F;

cy::Mat4 projection() noexcept {
    return cy::perspective_reversed_z(1.0471975512F, 1.0F, kNear, kFar);
}

}  // namespace

CY_TEST_CASE("the depth buffer clears to zero, which is the far plane") {
    // The first half of the convention, and the one a wrong clear breaks silently: with nothing
    // drawn, every texel must read exactly 0 — the value the FAR plane maps to.
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    ProbeImage image(fixture.allocator());
    CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(), projection(), image).has_value());

    cy::u32 wrong = 0;
    for (cy::u32 index = 0; index < cy::render_test::kProbeTexels; ++index) {
        if (image.depth[index] != 0.0F) {
            ++wrong;
        }
    }
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}

CY_TEST_CASE("the near plane maps to 1 and the far plane to 0, as sampled back from the device") {
    // THE ASSERTION design.md §3 ASKS FOR, and it is on the depth image's own texels rather than on
    // a matrix multiplication a test performed for itself.
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    ProbeImage image(fixture.allocator());
    const cy::u32 middle = cy::render_test::kProbeExtent / 2;

    // At the near plane: depth 1.
    const ProbeTriangle at_near = covering(kNear, 1.0F);
    CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(&at_near, 1), projection(), image)
                   .has_value());
    CY_CHECK_NEAR(image.depth_at(middle, middle), 1.0F, 1e-5F);

    // At the far plane: depth 0.
    const ProbeTriangle at_far = covering(kFar, 1.0F);
    CY_REQUIRE(
        fixture.render(cy::Span<const ProbeTriangle>(&at_far, 1), projection(), image).has_value());
    CY_CHECK_NEAR(image.depth_at(middle, middle), 0.0F, 1e-5F);

    // And in between, MONOTONICALLY DECREASING with distance. That is the direction of the whole
    // convention: nearer is greater, which is why the comparison is GreaterEqual.
    cy::f32 previous = 1.1F;
    for (const cy::f32 distance : {1.0F, 2.0F, 5.0F, 10.0F, 50.0F}) {
        const ProbeTriangle triangle = covering(distance, 1.0F);
        CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(&triangle, 1), projection(), image)
                       .has_value());
        const cy::f32 depth = image.depth_at(middle, middle);
        CY_CHECK_LT(depth, previous);
        CY_CHECK_GT(depth, 0.0F);
        CY_CHECK_LT(depth, 1.0F);
        previous = depth;
    }
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}

CY_TEST_CASE("the depth test keeps the nearer surface, whichever order the two are drawn in") {
    // THE CASE A GOLDEN IMAGE CANNOT MAKE. An inverted comparison renders a scene that looks
    // entirely correct until two surfaces overlap; here they overlap deliberately, and the winner
    // is asserted both ways round so that "it happened to be drawn last" is not the reason.
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    ProbeImage image(fixture.allocator());
    const cy::u32 middle = cy::render_test::kProbeExtent / 2;

    // Near is red (255), far is dark red (64). Whichever order they are submitted in, the near one
    // must win — and the depth left behind must be the near one's.
    const ProbeTriangle near_first[2] = {covering(2.0F, 1.0F), covering(20.0F, 0.25F)};
    CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(near_first, 2), projection(), image)
                   .has_value());
    CY_CHECK_GT(image.red_at(middle, middle), 200U);
    const cy::f32 depth_near_first = image.depth_at(middle, middle);

    const ProbeTriangle far_first[2] = {covering(20.0F, 0.25F), covering(2.0F, 1.0F)};
    CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(far_first, 2), projection(), image)
                   .has_value());
    CY_CHECK_GT(image.red_at(middle, middle), 200U);
    // The same depth either way: the test decided, not the order.
    CY_CHECK_NEAR(image.depth_at(middle, middle), depth_near_first, 1e-6F);

    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
    CY_CHECK_EQ(fixture.device().statistics().validation_warnings, 0U);
}

CY_TEST_CASE("an infinite far plane keeps depth in [0, 1] and never reaches the far value") {
    // `core-math`'s "Infinite far plane" is the mode reversed-Z makes reasonable rather than a
    // trick. Nothing is clipped at the back, and depth approaches 0 asymptotically without arriving
    // — so a very distant surface is still drawn, which a finite projection would have clipped
    // away.
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    const cy::Mat4 infinite = cy::perspective_reversed_z_infinite(1.0471975512F, 1.0F, kNear);
    ProbeImage image(fixture.allocator());
    const cy::u32 middle = cy::render_test::kProbeExtent / 2;

    const ProbeTriangle at_near = covering(kNear, 1.0F);
    CY_REQUIRE(
        fixture.render(cy::Span<const ProbeTriangle>(&at_near, 1), infinite, image).has_value());
    CY_CHECK_NEAR(image.depth_at(middle, middle), 1.0F, 1e-5F);

    // Ten kilometres out: drawn, with a depth that is small and strictly positive.
    const ProbeTriangle far_away = covering(10000.0F, 1.0F);
    CY_REQUIRE(
        fixture.render(cy::Span<const ProbeTriangle>(&far_away, 1), infinite, image).has_value());
    CY_CHECK_GT(image.red_at(middle, middle), 200U);
    CY_CHECK_GT(image.depth_at(middle, middle), 0.0F);
    CY_CHECK_LT(image.depth_at(middle, middle), 0.001F);
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}
