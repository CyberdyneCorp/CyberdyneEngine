// Handedness and axis conventions, verified against rendered output. Task 5.4.
//
// `core-math` fixes them: right-handed, **Y up**, **−Z forward**, **+X right**. M1 asserted that
// arithmetically. This file asserts it on an image, because the two can disagree — a viewport Y
// flip is a *backend* concern (`vulkan_command_buffer.cpp` writes a negative height), and a
// projection that is arithmetically correct still renders upside down if that line is wrong or
// missing.
//
// ================================================================================================
// THE IMAGE'S OWN COORDINATE SYSTEM, WHICH IS A THIRD ONE AND HAS TO BE SAID OUT LOUD
// ================================================================================================
//
// The readback buffer is row-major from the TOP-LEFT: row 0 is the top of the image. So a world
// point with a positive Y must land in a LOW row index, and a positive X in a high column index.
// Writing that down here is the difference between a test that checks the convention and a test
// that checks whichever way round its author happened to index the array.

#include "probe.h"

#include <cy/core/math/projection.h>
#include <cy/core/math/vec.h>

namespace {

using cy::render_test::kProbeExtent;
using cy::render_test::ProbeFixture;
using cy::render_test::ProbeImage;
using cy::render_test::ProbeTriangle;

/// A small triangle centred on `center`, in camera-relative space.
ProbeTriangle marker(cy::Vec3 center, cy::f32 red) noexcept {
    ProbeTriangle triangle;
    triangle.vertices[0] = center + cy::Vec3{-0.25F, -0.25F, 0.0F};
    triangle.vertices[1] = center + cy::Vec3{0.25F, -0.25F, 0.0F};
    triangle.vertices[2] = center + cy::Vec3{0.0F, 0.25F, 0.0F};
    triangle.color[0] = red;
    triangle.color[1] = 0.0F;
    triangle.color[2] = 0.0F;
    triangle.color[3] = 1.0F;
    return triangle;
}

/// The centroid of the drawn texels, in image coordinates. Row 0 is the TOP.
void centroid(const ProbeImage& image, cy::f32& out_x, cy::f32& out_y,
              cy::u32& out_count) noexcept {
    cy::f64 sum_x = 0.0;
    cy::f64 sum_y = 0.0;
    out_count = 0;
    for (cy::u32 y = 0; y < kProbeExtent; ++y) {
        for (cy::u32 x = 0; x < kProbeExtent; ++x) {
            if (image.red_at(x, y) > 32U) {
                sum_x += x;
                sum_y += y;
                ++out_count;
            }
        }
    }
    if (out_count == 0) {
        out_x = 0.0F;
        out_y = 0.0F;
        return;
    }
    out_x = static_cast<cy::f32>(sum_x / out_count);
    out_y = static_cast<cy::f32>(sum_y / out_count);
}

cy::Mat4 projection() noexcept {
    return cy::perspective_reversed_z(1.0471975512F, 1.0F, 0.1F, 100.0F);
}

/// One marker at `center`, rendered; its centroid returned. Returns false when nothing was drawn,
/// which is a failure the caller reports rather than a silent zero.
bool render_marker(ProbeFixture& fixture, ProbeImage& image, cy::Vec3 center, cy::f32& out_x,
                   cy::f32& out_y) noexcept {
    const ProbeTriangle triangle = marker(center, 1.0F);
    if (!fixture.render(cy::Span<const ProbeTriangle>(&triangle, 1), projection(), image)
             .has_value()) {
        return false;
    }
    cy::u32 count = 0;
    centroid(image, out_x, out_y, count);
    return count > 0;
}

}  // namespace

CY_TEST_CASE("the engine's axes land where the convention says, in the rendered image") {
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    ProbeImage image(fixture.allocator());
    const cy::f32 centre = static_cast<cy::f32>(kProbeExtent) * 0.5F;
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;

    // −Z IS FORWARD. A marker at −Z is in front of the camera and is drawn; the same marker at +Z
    // is behind it and is not. This is the assertion that fails first if a projection's sign is
    // wrong.
    CY_REQUIRE(render_marker(fixture, image, cy::Vec3{0.0F, 0.0F, -5.0F}, x, y));
    CY_CHECK_NEAR(x, centre, 1.5F);
    CY_CHECK_NEAR(y, centre, 1.5F);

    const ProbeTriangle behind = marker(cy::Vec3{0.0F, 0.0F, 5.0F}, 1.0F);
    CY_REQUIRE(
        fixture.render(cy::Span<const ProbeTriangle>(&behind, 1), projection(), image).has_value());
    cy::u32 drawn = 0;
    cy::f32 ignored_x = 0.0F;
    cy::f32 ignored_y = 0.0F;
    centroid(image, ignored_x, ignored_y, drawn);
    CY_CHECK_EQ(drawn, 0U);

    // +X IS RIGHT: a higher column index.
    CY_REQUIRE(render_marker(fixture, image, cy::Vec3{1.5F, 0.0F, -5.0F}, x, y));
    CY_CHECK_GT(x, centre + 5.0F);
    CY_CHECK_NEAR(y, centre, 1.5F);

    // +Y IS UP, and the image's row 0 is the TOP — so up is a LOWER row index. This is the one the
    // viewport Y flip decides, and it is why the convention is checked against an image at all.
    CY_REQUIRE(render_marker(fixture, image, cy::Vec3{0.0F, 1.5F, -5.0F}, x, y));
    CY_CHECK_NEAR(x, centre, 1.5F);
    CY_CHECK_LT(y, centre - 5.0F);

    // And the mirror images, so that "greater than centre" is not satisfied by everything drifting
    // one way.
    CY_REQUIRE(render_marker(fixture, image, cy::Vec3{-1.5F, 0.0F, -5.0F}, x, y));
    CY_CHECK_LT(x, centre - 5.0F);
    CY_REQUIRE(render_marker(fixture, image, cy::Vec3{0.0F, -1.5F, -5.0F}, x, y));
    CY_CHECK_GT(y, centre + 5.0F);

    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}

CY_TEST_CASE("the basis is right-handed on the device, not only in the arithmetic") {
    // Right-handed means `cross(right, up) == -forward`: with +X right and +Y up, the cross product
    // points along +Z, which is BEHIND the camera. The image test above pins each axis separately;
    // this pins the relationship between them, which is what "handedness" actually names.
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    const cy::Vec3 right = cy::kAxisRight;
    const cy::Vec3 up = cy::kAxisUp;
    const cy::Vec3 forward = cy::kAxisForward;
    const cy::Vec3 product = cross(right, up);
    CY_CHECK_NEAR(product.x, -forward.x, 1e-6F);
    CY_CHECK_NEAR(product.y, -forward.y, 1e-6F);
    CY_CHECK_NEAR(product.z, -forward.z, 1e-6F);

    // The rendered half: a marker displaced along `cross(right, up)` is behind the camera and draws
    // nothing, while one displaced along `forward` is in front and draws. If the engine were
    // left-handed those two would swap, and every axis case above would still pass.
    ProbeImage image(fixture.allocator());
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    CY_CHECK(render_marker(fixture, image, forward * 5.0F, x, y));
    CY_CHECK_FALSE(render_marker(fixture, image, product * 5.0F, x, y));
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}

CY_TEST_CASE("a nearer marker occludes a further one, in the image the device produced") {
    // The conventions meeting each other: −Z forward, reversed-Z depth, GreaterEqual. Two markers
    // at the same screen position and different depths — the nearer one's colour must be what the
    // image holds, and its depth must be what the depth buffer holds.
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    ProbeImage image(fixture.allocator());
    const cy::u32 middle = kProbeExtent / 2;

    // The far marker alone, so the comparison below is against a measured depth rather than against
    // a number this test worked out for itself from the projection it is supposed to be checking.
    const ProbeTriangle far_only = marker(cy::Vec3{0.0F, 0.0F, -20.0F}, 0.25F);
    CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(&far_only, 1), projection(), image)
                   .has_value());
    const cy::f32 far_depth = image.depth_at(middle, middle);
    CY_CHECK_GT(far_depth, 0.0F);

    const ProbeTriangle pair[2] = {far_only, marker(cy::Vec3{0.0F, 0.0F, -3.0F}, 1.0F)};
    CY_REQUIRE(
        fixture.render(cy::Span<const ProbeTriangle>(pair, 2), projection(), image).has_value());

    // The nearer marker's colour survived...
    CY_CHECK_GT(image.red_at(middle, middle), 200U);
    // ...and so did its depth. Nearer is GREATER under reversed-Z, which is the direction that
    // makes the comparison GreaterEqual rather than Less.
    CY_CHECK_GT(image.depth_at(middle, middle), far_depth);
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}
