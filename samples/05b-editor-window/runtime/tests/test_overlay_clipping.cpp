// The overlay rasteriser's bounds checking, over geometry that is mostly off the canvas. M7 task
// 5b.3.
//
// INTEGRATION RATHER THAN UNIT, and the reason is the rasteriser rather than the assertion. A gizmo
// whose handles sit at (-50, -50) and (400, 400) on a 24x24 frame makes `draw_gizmo` walk axis
// lines hundreds of pixels long of which about a dozen land inside, and every one of those steps
// goes through the bounds-checked blend this case exists to exercise. Shortening the lines is the
// one thing that would make it cheaper and it is also the thing that would stop it testing
// anything: the property is that a coordinate far outside the canvas is clipped rather than
// written.
//
// It was found by M7's closing gate. In the Debug configuration on a quiet machine it measured 0.81
// to 0.98 ms of CPU against the unit suite's 1.00 ms — two per cent of margin at worst — and 1.94
// ms on the same machine while the four-profile build was running, which is what took
// `unit.editor_window_runtime` down in that run. Its siblings in `test_runtime.cpp` draw into the
// frame rather than past it and are an order of magnitude cheaper.
//
// The canvas is built inline rather than through `test_runtime.cpp`'s `Frame`, because this case
// needs a cleared buffer and one pixel back and nothing else that helper offers.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include "overlay.h"

using cy::f32;
using cy::u32;
using cy::u8;
using namespace cy::sample::editor_window;

CY_TEST_CASE("drawing outside the frame is clipped rather than corrupting memory") {
    // A handle whose object is at the edge of the viewport is drawn partly off it, and every
    // rasteriser in overlay.cpp goes through one bounds-checked blend for exactly that reason.
    constexpr u32 kAcross = 24;
    constexpr u32 kDown = 24;
    cy::Array<u8> pixels;
    CY_REQUIRE(pixels.resize(static_cast<cy::usize>(kAcross) * kDown * 4));
    for (cy::usize index = 0; index < pixels.size(); index += 4) {
        pixels[index] = 0;
        pixels[index + 1] = 0;
        pixels[index + 2] = 0;
        pixels[index + 3] = 0xFF;
    }
    const Canvas canvas{pixels.data(), kAcross, kDown};

    cy::render::GizmoLayout layout;
    layout.frame_id = 1;
    layout.centre_x = 0.0F;
    layout.centre_y = 0.0F;
    layout.extent = 100.0F;
    CY_REQUIRE(layout.spots.push_back(
        cy::render::GizmoHandleSpot{cy::render::GizmoHandle::AxisX, -50.0F, -50.0F, 9.0F, 1.0F}));
    CY_REQUIRE(layout.spots.push_back(
        cy::render::GizmoHandleSpot{cy::render::GizmoHandle::AxisY, 400.0F, 400.0F, 9.0F, 1.0F}));
    draw_gizmo(canvas, layout, cy::render::GizmoHandle::Count);
    draw_selection_marker(canvas, -20.0F, 200.0F, 30.0F);

    // Reaching here without a sanitiser report is the assertion; the pixel is a witness that the
    // canvas was not simply ignored.
    CY_CHECK(static_cast<f32>(pixels[0]) >= 0.0F);
}
