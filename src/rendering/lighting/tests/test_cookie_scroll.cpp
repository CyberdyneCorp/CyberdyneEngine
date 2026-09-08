// A scrolling directional cookie, over long enough for the drift this case exists to catch. Task
// 10.3.
//
// INTEGRATION RATHER THAN UNIT, and the reason is the loop rather than the subject. The property is
// that `advance_cookie_scroll` wraps into [0, 1) and never grows, which an unwrapped f32 UV fails
// only after it has accumulated enough additions to reach its own quantisation — so the case runs
// 20,000 frames, about five and a half minutes of play at 60 Hz. Shortening the loop to fit a unit
// budget would leave a case that passes with the wrap removed, which is testing something else.
//
// It was found by M7's closing gate rather than by a person reading: in the Debug configuration on
// a quiet machine it measured 0.87 to 0.95 ms of CPU against the unit suite's 1.00 ms, and 1.71 ms
// on the same machine while the four-profile build was running. A case with five per cent of margin
// in one configuration is a case that reports a machine rather than a regression, and
// `testing-and-quality`'s taxonomy has a suite for exactly that. Its siblings in
// `test_many_light.cpp` are all an order of magnitude cheaper and stay where they are.

#include <cy/test/test.h>

#include <cy/rendering/lighting/light_functions.h>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec2;
using cy::Vec3;
using cy::rendering::advance_cookie_scroll;
using cy::rendering::cookie_uv;
using cy::rendering::CookieProjection;
using cy::rendering::CookieProjectionKind;

}  // namespace

CY_TEST_CASE(
    "cookies: a directional cloud cookie is the same at any altitude and costs no shadow") {
    CookieProjection clouds;
    clouds.kind = CookieProjectionKind::OrthographicPlane;
    clouds.forward = Vec3{0.0F, 0.0F, -1.0F};
    clouds.right = Vec3{1.0F, 0.0F, 0.0F};
    clouds.up = Vec3{0.0F, 1.0F, 0.0F};
    clouds.world_scale = 500.0F;
    clouds.tile = true;

    // A directional light has no position, so two points on the same vertical line get the same
    // cookie value — which is what makes this a cloud SHADOW and not a projector.
    const auto low = cookie_uv(clouds, Vec3{120.0F, 40.0F, 0.0F});
    const auto high = cookie_uv(clouds, Vec3{120.0F, 40.0F, 900.0F});
    CY_CHECK_NEAR(low.uv.x, high.uv.x, 1.0e-6F);
    CY_CHECK_NEAR(low.uv.y, high.uv.y, 1.0e-6F);

    // Scrolling moves it, wraps into [0, 1), and never grows: an unwrapped f32 UV drifts into its
    // own quantisation after an hour of play and the pattern visibly steps.
    for (u32 frame = 0; frame < 20000; ++frame) {
        advance_cookie_scroll(clouds, Vec2{0.1F, 0.05F}, Vec3{}, 1.0F / 60.0F);
    }
    CY_CHECK_GE(clouds.scroll_uv.x, 0.0F);
    CY_CHECK_LT(clouds.scroll_uv.x, 1.0F);
    CY_CHECK_GE(clouds.scroll_uv.y, 0.0F);
    CY_CHECK_LT(clouds.scroll_uv.y, 1.0F);

    // And a tiling cookie is inside everywhere, so a scrolling cloud layer never leaves a hole.
    CY_CHECK(cookie_uv(clouds, Vec3{-9000.0F, 12000.0F, 0.0F}).inside);
}
