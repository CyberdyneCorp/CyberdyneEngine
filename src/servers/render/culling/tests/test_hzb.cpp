// The hierarchical depth buffer, its conservatism, and the two-pass scheme. M6 task 8.5.
//
// The pyramid is filled by hand here rather than by a device, which is what makes the occlusion
// path assertable in every profile on a machine with no GPU — and it is why `Hzb` holds an array of
// floats rather than a texture.

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/culling/hzb.h>
#include <cy/test/test.h>

using namespace cy::render::culling;
using cy::f32;
using cy::u32;
using cy::Vec3;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

cy::Mat4 view_projection() {
    return cy::perspective_reversed_z(1.0471975512F, 1.0F, 0.1F, 1000.0F) *
           cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
}

/// Fill level 0 with one depth everywhere and build the pyramid from it.
void fill(Hzb& pyramid, f32 depth) {
    for (f32& texel : pyramid.level(0)) {
        texel = depth;
    }
    pyramid.reduce();
    pyramid.mark_valid();
}

}  // namespace

CY_TEST_CASE("hzb: a pyramid has levels down to a single texel") {
    CY_CHECK(hzb_level_count(1, 1) == 1);
    CY_CHECK(hzb_level_count(2, 2) == 2);
    CY_CHECK(hzb_level_count(64, 64) == 7);
    // A non-square buffer keeps halving until BOTH dimensions are one, so the coarsest level is a
    // texel and not a row.
    CY_CHECK(hzb_level_count(64, 16) == 7);
    CY_CHECK(hzb_level_count(0, 16) == 0);

    Hzb pyramid(allocator());
    CY_REQUIRE(pyramid.resize(64, 16).has_value());
    CY_CHECK(pyramid.level_count() == 7);
    CY_CHECK(pyramid.level_width(0) == 64);
    CY_CHECK(pyramid.level_height(0) == 16);
    CY_CHECK(pyramid.level_width(6) == 1);
    CY_CHECK(pyramid.level_height(6) == 1);
    CY_CHECK(pyramid.level(6).size() == 1);
}

CY_TEST_CASE("hzb: the reduction keeps the FURTHEST depth of each footprint") {
    // Under reversed Z the furthest depth is the SMALLEST. Getting this backwards produces a
    // renderer that culls what is visible and draws what is not.
    Hzb pyramid(allocator());
    CY_REQUIRE(pyramid.resize(4, 4).has_value());
    const cy::Span<f32> base = pyramid.level(0);
    for (f32& texel : base) {
        texel = 0.9F;
    }
    // One distant texel in the first 2x2 footprint.
    base[0] = 0.1F;
    pyramid.reduce();
    CY_CHECK(pyramid.level(1)[0] == 0.1F);
    CY_CHECK(pyramid.level(1)[1] == 0.9F);
    // And it survives to the top: the coarsest texel is the furthest thing anywhere.
    CY_CHECK(pyramid.level(2)[0] == 0.1F);
}

CY_TEST_CASE("hzb: an object behind the depth buffer is occluded and one in front is not") {
    Hzb pyramid(allocator());
    CY_REQUIRE(pyramid.resize(64, 64).has_value());
    const cy::Mat4 vp = view_projection();

    // A wall drawn at 10 metres. A SMALL sphere on purpose: `nearest_depth` is taken at the
    // sphere's closest point, so a radius of 8 would put the occluder's depth at 2 metres and the
    // test would be measuring the fixture rather than the pyramid.
    const ScreenRect wall = project_sphere(Vec3{0.0F, 0.0F, -10.0F}, 0.1F, vp, 64, 64);
    CY_REQUIRE(wall.valid);
    fill(pyramid, wall.nearest_depth);

    // Something at 50 metres, behind it, is occluded.
    const ScreenRect behind = project_sphere(Vec3{0.0F, 0.0F, -50.0F}, 1.0F, vp, 64, 64);
    CY_REQUIRE(behind.valid);
    CY_CHECK(pyramid.occludes(behind));

    // Something at 5 metres, in front of it, is not.
    const ScreenRect front = project_sphere(Vec3{0.0F, 0.0F, -5.0F}, 1.0F, vp, 64, 64);
    CY_REQUIRE(front.valid);
    CY_CHECK(!pyramid.occludes(front));
}

CY_TEST_CASE("hzb: an invalid pyramid occludes nothing, which is what a camera cut leaves") {
    // "WHEN the camera teleports THEN the previous frame's depth SHALL be discarded and no
    // occlusion culling SHALL be applied for that frame."
    Hzb pyramid(allocator());
    CY_REQUIRE(pyramid.resize(32, 32).has_value());
    const cy::Mat4 vp = view_projection();
    const ScreenRect far_away = project_sphere(Vec3{0.0F, 0.0F, -80.0F}, 1.0F, vp, 32, 32);

    fill(pyramid, 0.95F);
    CY_CHECK(pyramid.valid());
    CY_CHECK(pyramid.occludes(far_away));

    pyramid.invalidate();
    CY_CHECK(!pyramid.valid());
    CY_CHECK(!pyramid.occludes(far_away));

    // A resize invalidates too: the previous frame's depth means nothing at a new resolution.
    fill(pyramid, 0.95F);
    CY_REQUIRE(pyramid.resize(16, 16).has_value());
    CY_CHECK(!pyramid.valid());
}

CY_TEST_CASE("hzb: a sphere the camera is inside, or behind it, is never occluded") {
    // Both cases are "not known", and not known must read as "draw it".
    Hzb pyramid(allocator());
    CY_REQUIRE(pyramid.resize(32, 32).has_value());
    fill(pyramid, 0.99F);
    const cy::Mat4 vp = view_projection();

    const ScreenRect straddling = project_sphere(Vec3{0.0F, 0.0F, -0.05F}, 1.0F, vp, 32, 32);
    CY_CHECK(!straddling.valid);
    CY_CHECK(!pyramid.occludes(straddling));

    const ScreenRect behind_eye = project_sphere(Vec3{0.0F, 0.0F, 20.0F}, 1.0F, vp, 32, 32);
    CY_CHECK(!behind_eye.valid);
    CY_CHECK(!pyramid.occludes(behind_eye));

    // And one entirely off screen: the frustum test has already had its say, so this one does not
    // need to guess.
    const ScreenRect off_screen = project_sphere(Vec3{500.0F, 0.0F, -10.0F}, 1.0F, vp, 32, 32);
    CY_CHECK(!pyramid.occludes(off_screen));
}

CY_TEST_CASE("hzb: a larger footprint is tested at a coarser level") {
    // The level is chosen so at most a 2x2 of texels covers the rectangle, which is what keeps the
    // test four taps rather than a loop over the silhouette.
    ScreenRect tiny;
    tiny.valid = true;
    tiny.max_x = 1.0F;
    tiny.max_y = 1.0F;
    CY_CHECK(hzb_level_for(tiny, 8) == 0);

    ScreenRect wide;
    wide.valid = true;
    wide.max_x = 64.0F;
    wide.max_y = 8.0F;
    CY_CHECK(hzb_level_for(wide, 8) == 6);

    // Never past the top of the pyramid.
    ScreenRect enormous;
    enormous.valid = true;
    enormous.max_x = 100000.0F;
    enormous.max_y = 100000.0F;
    CY_CHECK(hzb_level_for(enormous, 8) == 7);
}

CY_TEST_CASE("hzb: the tester wires a pyramid into the cull's occlusion stage") {
    Hzb pyramid(allocator());
    CY_REQUIRE(pyramid.resize(64, 64).has_value());
    const cy::Mat4 vp = view_projection();
    const ScreenRect wall = project_sphere(Vec3{0.0F, 0.0F, -10.0F}, 0.1F, vp, 64, 64);
    fill(pyramid, wall.nearest_depth);

    const HzbOcclusionTester tester(pyramid, vp);
    CY_CHECK(tester.occluded(Vec3{0.0F, 0.0F, -50.0F}, 1.0F));
    CY_CHECK(!tester.occluded(Vec3{0.0F, 0.0F, -5.0F}, 1.0F));
}

CY_TEST_CASE(
    "hzb: visibility hysteresis keeps a recently seen instance for the configured frames") {
    VisibilityHistory history(allocator());
    CY_REQUIRE(history.resize(8).has_value());

    // Nothing has been seen, so nothing is recent.
    CY_CHECK(!history.recently_visible(3, 4));

    history.mark_visible(3);
    CY_CHECK(history.recently_visible(3, 4));
    for (u32 frame = 0; frame < 4; ++frame) {
        history.advance();
    }
    CY_CHECK(history.recently_visible(3, 4));
    history.advance();
    CY_CHECK(!history.recently_visible(3, 4));

    // Zero frames disables it, which is what a deterministic test and a golden image need.
    history.mark_visible(3);
    CY_CHECK(!history.recently_visible(3, 0));
}

CY_TEST_CASE("hzb: the two-pass scheme draws last frame's visible set first") {
    // "a first pass drawing what previous-frame visibility suggests, an HZB rebuilt from that
    // depth, and a second pass testing uncertain or newly visible geometry".
    TwoPassCull two_pass(allocator());
    CY_REQUIRE(two_pass.resize(8).has_value());

    // A first frame has no history, so the first pass draws nothing and everything falls to the
    // second — which is the correct, conservative behaviour.
    CY_CHECK(two_pass.first_pass_count() == 0);
    CY_CHECK(!two_pass.in_first_pass(2));

    GpuDrawPayload payloads[2];
    payloads[0].instance_slot = 2;
    payloads[1].instance_slot = 5;
    two_pass.record_visible(cy::Span<const GpuDrawPayload>(payloads, 2));
    two_pass.advance();

    CY_CHECK(two_pass.first_pass_count() == 2);
    CY_CHECK(two_pass.in_first_pass(2));
    CY_CHECK(two_pass.in_first_pass(5));
    CY_CHECK(!two_pass.in_first_pass(4));

    // A frame in which nothing is visible clears the first pass rather than keeping the old set.
    two_pass.advance();
    CY_CHECK(two_pass.first_pass_count() == 0);
    CY_CHECK(!two_pass.in_first_pass(2));

    // A camera cut forgets everything, alongside `Hzb::invalidate()`.
    two_pass.record_visible(cy::Span<const GpuDrawPayload>(payloads, 2));
    two_pass.advance();
    two_pass.clear();
    CY_CHECK(!two_pass.in_first_pass(2));
    CY_CHECK(two_pass.first_pass_count() == 0);
}
