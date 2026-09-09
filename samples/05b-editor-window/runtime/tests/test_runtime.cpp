// The engine-side runtime's wire and its one drawing. M7 tasks 5b.3 and 5b.4; M8.a task 1.4.
//
// Everything here is testable without a device, which is why it is in its own translation units
// rather than in `main.cpp`: the decoding of a pick the editor sent, the encoding of the answer,
// and the drawing of a published layout into a frame. The Vulkan is `smoke.editor_window`'s to
// exercise, because it needs a display and an editor anyway; the world itself is
// `integration.editor_window_one_world`'s, because building a scene costs more than a millisecond.
//
// **WHAT LEFT THIS FILE AT M8.a.** The association between an editor identity and one of this
// scene's objects, and the transaction decoder that went with it. Both were `session.h`, both were
// a stand-in for a shared world, and there is a shared world now: see `world_view.h` and
// `cy/scene/serialization/worldfile.h`.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/picking.h>
#include <cy/test/test.h>

#include "overlay.h"
#include "pick_wire.h"

#include <cstring>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::u8;
using namespace cy::sample::editor_window;

namespace {

void little_endian(cy::Array<u8>& out, u64 value, cy::usize width) {
    for (cy::usize index = 0; index < width; ++index) {
        CY_REQUIRE(out.push_back(static_cast<u8>((value >> (index * 8)) & 0xFFU)));
    }
}

void float_value(cy::Array<u8>& out, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    little_endian(out, bits, 4);
}

/// A canvas of `width * height` opaque black pixels.
struct Frame {
    cy::Array<u8> pixels;
    u32 width = 0;
    u32 height = 0;

    Frame(u32 across, u32 down) : width(across), height(down) {
        CY_REQUIRE(pixels.resize(static_cast<cy::usize>(across) * down * 4));
        for (cy::usize index = 0; index < pixels.size(); index += 4) {
            pixels[index] = 0;
            pixels[index + 1] = 0;
            pixels[index + 2] = 0;
            pixels[index + 3] = 0xFF;
        }
    }

    [[nodiscard]] Canvas canvas() noexcept { return Canvas{pixels.data(), width, height}; }

    [[nodiscard]] cy::Vec3 at(u32 x, u32 y) const noexcept {
        const cy::usize index = ((static_cast<cy::usize>(y) * width) + x) * 4;
        return cy::Vec3{static_cast<f32>(pixels[index]), static_cast<f32>(pixels[index + 1]),
                        static_cast<f32>(pixels[index + 2])};
    }

    /// The pixel nearest (x, y) whose colour is strongest, within a small box. A handle is drawn
    /// with a soft edge, so the exact centre is not necessarily the most saturated texel.
    [[nodiscard]] cy::Vec3 strongest_near(u32 x, u32 y, u32 reach) const noexcept {
        cy::Vec3 best{0.0F, 0.0F, 0.0F};
        f32 best_sum = -1.0F;
        for (u32 dy = 0; dy <= reach * 2; ++dy) {
            for (u32 dx = 0; dx <= reach * 2; ++dx) {
                const u32 px = x + dx - reach;
                const u32 py = y + dy - reach;
                if (px >= width || py >= height) {
                    continue;
                }
                const cy::Vec3 pixel = at(px, py);
                const f32 sum = pixel.x + pixel.y + pixel.z;
                if (sum > best_sum) {
                    best_sum = sum;
                    best = pixel;
                }
            }
        }
        return best;
    }
};

/// The canvas every drawing case uses.
///
/// SMALL ON PURPOSE. The rasteriser is per-pixel and the shafts are drawn as runs of discs, so a
/// 256x192 frame with three full-length arrows in it costs about two milliseconds — over the unit
/// suite's budget, and it failed one run in a dozen while passing on its own. 96x72 exercises every
/// branch and costs a tenth of that. `testing-and-quality`'s taxonomy is what decides this, not the
/// convenience of a round number.
constexpr u32 kCanvasWidth = 96;
constexpr u32 kCanvasHeight = 72;

[[nodiscard]] cy::render::GizmoLayout a_layout() {
    cy::render::GizmoLayout layout;
    layout.frame_id = 7;
    layout.mode = cy::render::GizmoMode::Translate;
    layout.centre_x = 48.0F;
    layout.centre_y = 36.0F;
    layout.extent = 22.0F;
    const cy::render::GizmoHandleSpot spots[] = {
        {cy::render::GizmoHandle::AxisX, 70.0F, 36.0F, 5.0F, 6.0F},
        {cy::render::GizmoHandle::AxisY, 48.0F, 14.0F, 5.0F, 6.0F},
        {cy::render::GizmoHandle::AxisZ, 26.0F, 36.0F, 5.0F, 6.0F},
    };
    for (const cy::render::GizmoHandleSpot& spot : spots) {
        CY_REQUIRE(layout.spots.push_back(spot));
    }
    return layout;
}

}  // namespace

// --- the pick wire, M8.a task 1.4 -------------------------------------------------------------

/// A `PickRequest` as `cy_editor_viewport::picking::PickRequest::encode` writes one.
[[nodiscard]] cy::Array<u8> a_click(u64 frame, f32 x, f32 y) {
    cy::Array<u8> out;
    little_endian(out, 3, 8);      // the viewport
    little_endian(out, frame, 8);  // the frame that was on screen
    CY_REQUIRE(out.push_back(0));  // PickIntent::Click
    float_value(out, x);
    float_value(out, y);
    little_endian(out, 0xFFFFFFFFULL, 4);  // layers
    CY_REQUIRE(out.push_back(1));          // include_transparent
    little_endian(out, 16, 4);             // max_candidates
    little_endian(out, 1, 4);              // one excluded identity
    little_endian(out, 0xDEADULL, 8);
    return out;
}

CY_TEST_CASE("a click the editor sent decodes to the pixel it named") {
    const cy::Array<u8> bytes = a_click(42, 640.5F, 360.25F);
    PickRequest request(cy::system_allocator(cy::MemoryDomain::Gpu));
    CY_REQUIRE(decode_pick_request(cy::Span<const u8>{bytes.data(), bytes.size()}, request));
    CY_CHECK_EQ(request.viewport, 3ULL);
    CY_CHECK_EQ(request.frame, 42ULL);
    CY_CHECK(request.kind == PickKind::Click);
    CY_CHECK_NEAR(request.x, 640.5F, 1e-6F);
    CY_CHECK_NEAR(request.y, 360.25F, 1e-6F);
    CY_CHECK_EQ(request.max_candidates, 16U);
    CY_REQUIRE_EQ(request.excluded.size(), 1U);
    CY_CHECK_EQ(request.excluded[0], 0xDEADULL);
}

CY_TEST_CASE("a truncated pick is refused at every length rather than half read") {
    // The same rule the transaction decoder follows, and for the same reason: a request read past
    // its end resolves against a misread filter, which is a confident wrong answer.
    const cy::Array<u8> bytes = a_click(7, 1.0F, 2.0F);
    for (cy::usize length = 0; length + 1 < bytes.size(); ++length) {
        PickRequest request(cy::system_allocator(cy::MemoryDomain::Gpu));
        CY_CHECK_FALSE(decode_pick_request(cy::Span<const u8>{bytes.data(), length}, request));
    }
}

CY_TEST_CASE("a pick intent from a newer editor is refused by name") {
    cy::Array<u8> bytes;
    little_endian(bytes, 1, 8);
    little_endian(bytes, 1, 8);
    CY_REQUIRE(bytes.push_back(9));  // an intent kind that does not exist
    PickRequest request(cy::system_allocator(cy::MemoryDomain::Gpu));
    CY_CHECK_FALSE(decode_pick_request(cy::Span<const u8>{bytes.data(), bytes.size()}, request));
}

CY_TEST_CASE("an answer encodes as the editor reads it") {
    // `PickResponse::decode`: the frame, a count, then identity, distance and a transparency byte.
    cy::Array<cy::render::PickCandidate> candidates;
    cy::render::PickCandidate near;
    near.stable_id = 0x1122'3344'5566'7788ULL;
    near.distance = 2.5F;
    CY_REQUIRE(candidates.push_back(near));

    cy::Array<u8> reply;
    CY_REQUIRE(encode_pick_response(42, candidates.span(), reply));
    CY_REQUIRE_EQ(reply.size(), cy::usize{8 + 4 + 8 + 4 + 1});
    CY_CHECK_EQ(reply[0], 42U);
    CY_CHECK_EQ(reply[8], 1U);  // one candidate
    CY_CHECK_EQ(reply[12], 0x88U);
    CY_CHECK_EQ(reply[19], 0x11U);
    CY_CHECK_EQ(reply[24], 0U);  // opaque
}

CY_TEST_CASE("a click on nothing is an empty answer rather than a failure") {
    // "The user clicked the sky" is not an error, and the editor's `Replace` mode clears the
    // selection on one. An empty list has to encode.
    cy::Array<u8> reply;
    CY_REQUIRE(encode_pick_response(9, {}, reply));
    CY_REQUIRE_EQ(reply.size(), cy::usize{12});
    CY_CHECK_EQ(reply[8], 0U);
}

CY_TEST_CASE("the axis triad is drawn in the editor's own colours") {
    // REGRESSION, and the first thing a screenshot showed: the constants are 0xRRGGBB and the
    // canvas is RGBA, so channel 0 is the HIGH byte. Indexed the other way round the X axis is
    // drawn blue and the Z axis orange — a mistake that is invisible in a diff and unmistakable in
    // a picture, and one that breaks the single colour convention `editor-visual-language` shares
    // with every other tool.
    Frame frame(kCanvasWidth, kCanvasHeight);
    draw_gizmo(frame.canvas(), a_layout(), cy::render::GizmoHandle::Count);

    const cy::Vec3 x_arrow = frame.strongest_near(70, 36, 3);
    CY_CHECK(x_arrow.x > 200.0F);
    CY_CHECK(x_arrow.y < 130.0F);
    CY_CHECK(x_arrow.z < 130.0F);

    const cy::Vec3 y_arrow = frame.strongest_near(48, 14, 3);
    CY_CHECK(y_arrow.y > 170.0F);
    CY_CHECK(y_arrow.x < 130.0F);

    const cy::Vec3 z_arrow = frame.strongest_near(26, 36, 3);
    CY_CHECK(z_arrow.z > 200.0F);
    CY_CHECK(z_arrow.x < 130.0F);
}

CY_TEST_CASE("what is drawn is where the layout says it is") {
    // The property the whole division of labour rests on: the editor hit-tests the published
    // layout, so a handle drawn anywhere else is a handle the user cannot grab.
    Frame frame(kCanvasWidth, kCanvasHeight);
    draw_gizmo(frame.canvas(), a_layout(), cy::render::GizmoHandle::Count);
    // Ink at the handle...
    CY_CHECK(frame.at(70, 36).x > 100.0F);
    // ...and none in a corner the gizmo does not reach.
    const cy::Vec3 corner = frame.at(kCanvasWidth - 2, kCanvasHeight - 2);
    CY_CHECK_NEAR(corner.x + corner.y + corner.z, 0.0F, 1e-6F);
}

CY_TEST_CASE("an emphasised handle is lifted, never recoloured") {
    // `editor-visual-language`: the active state is a luminance and saturation lift. Recolouring
    // would break the axis mapping, which is the one convention shared with every other tool — so
    // the emphasised X is a BRIGHTER X and still unmistakably red.
    Frame resting(kCanvasWidth, kCanvasHeight);
    draw_gizmo(resting.canvas(), a_layout(), cy::render::GizmoHandle::Count);
    Frame active(kCanvasWidth, kCanvasHeight);
    draw_gizmo(active.canvas(), a_layout(), cy::render::GizmoHandle::AxisX);

    const cy::Vec3 quiet = resting.strongest_near(70, 36, 3);
    const cy::Vec3 lifted = active.strongest_near(70, 36, 3);
    CY_CHECK(lifted.x + lifted.y + lifted.z > quiet.x + quiet.y + quiet.z);
    CY_CHECK(lifted.x > lifted.y);
    CY_CHECK(lifted.x > lifted.z);
    // And the OTHER axes are untouched, because emphasis is about one handle.
    const cy::Vec3 y_quiet = resting.strongest_near(48, 14, 3);
    const cy::Vec3 y_active = active.strongest_near(48, 14, 3);
    CY_CHECK_NEAR(y_quiet.y, y_active.y, 1e-6F);
}

CY_TEST_CASE("an empty layout draws nothing at all") {
    Frame frame(48, 48);
    cy::render::GizmoLayout empty;
    draw_gizmo(frame.canvas(), empty, cy::render::GizmoHandle::Count);
    f32 total = 0.0F;
    for (u32 y = 0; y < 48; ++y) {
        for (u32 x = 0; x < 48; ++x) {
            const cy::Vec3 pixel = frame.at(x, y);
            total += pixel.x + pixel.y + pixel.z;
        }
    }
    CY_CHECK_NEAR(total, 0.0F, 1e-6F);
}

// `drawing outside the frame is clipped rather than corrupting memory` was here and is now in
// test_overlay_clipping.cpp, in the integration suite. Walking axis lines that are hundreds of
// pixels long past a 24x24 canvas measured 0.81 to 0.98 ms of CPU in the Debug configuration
// against a 1.00 ms unit budget. That file carries the measurement.

CY_TEST_CASE("a null canvas is a no-op rather than a crash") {
    const Canvas nothing;
    draw_gizmo(nothing, a_layout(), cy::render::GizmoHandle::Count);
    draw_selection_marker(nothing, 1.0F, 1.0F, 4.0F);
    CY_CHECK(nothing.pixels == nullptr);
}
