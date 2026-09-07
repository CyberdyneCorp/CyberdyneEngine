// The engine-side runtime's two decisions and its one drawing. M7 tasks 5b.1, 5b.3 and 5b.4.
//
// Everything here is testable without a device, which is why it is in its own translation units
// rather than in `main.cpp`: the association between an editor's identity and one of this scene's
// objects, the reading of a transaction the editor committed, and the drawing of a published layout
// into a frame. The Vulkan is `smoke.editor_window`'s to exercise, because it needs a display and
// an editor anyway.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include "overlay.h"
#include "session.h"

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

void text(cy::Array<u8>& out, const char* value) {
    const cy::usize length = std::strlen(value);
    little_endian(out, length, 4);
    for (cy::usize index = 0; index < length; ++index) {
        CY_REQUIRE(out.push_back(static_cast<u8>(value[index])));
    }
}

void float_value(cy::Array<u8>& out, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    little_endian(out, bits, 4);
}

/// A transaction as `cy_editor_documents::transaction::Transaction::encode` writes one.
///
/// Written out by hand rather than captured, so that a change to the editor's encoding fails here
/// with a sentence about the field that moved rather than as a runtime that stopped applying
/// moves — which is how this decoder's first defect presented.
[[nodiscard]] cy::Array<u8> a_move(u64 node, f32 from_x, f32 to_x) {
    cy::Array<u8> bytes;
    little_endian(bytes, 0x1234, 8);  // the transaction's identity
    little_endian(bytes, node, 8);    // the document's, as two halves
    little_endian(bytes, 0, 8);
    text(bytes, "Translate X");      // its name
    CY_REQUIRE(bytes.push_back(0));  // Actor::Human
    text(bytes, "designer");
    CY_REQUIRE(bytes.push_back(1));  // a coalesce key follows
    text(bytes, "gizmo-drag");
    little_endian(bytes, 1, 4);      // one operation
    CY_REQUIRE(bytes.push_back(6));  // Operation::SetField
    little_endian(bytes, node, 8);   // the node, as two halves
    little_endian(bytes, 0, 8);
    little_endian(bytes, 3, 8);      // the component
    little_endian(bytes, 4, 8);      // the field
    CY_REQUIRE(bytes.push_back(6));  // Value::Vec3, before
    float_value(bytes, from_x);
    float_value(bytes, 0.0F);
    float_value(bytes, 0.0F);
    CY_REQUIRE(bytes.push_back(6));  // Value::Vec3, after
    float_value(bytes, to_x);
    float_value(bytes, 0.0F);
    float_value(bytes, 0.0F);
    return bytes;
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

CY_TEST_CASE("an identity keeps the object it was first given") {
    // The association `session.h` explains: a stand-in for a shared world, and the property that
    // makes it usable is that it does not move. A gizmo that landed on a different object every
    // frame would be worse than no gizmo.
    EditorSession session;
    const u32 first = session.object_for(0xAAAA, 6);
    CY_CHECK_EQ(session.object_for(0xAAAA, 6), first);
    CY_CHECK_NE(session.object_for(0xBBBB, 6), first);
    CY_CHECK_EQ(session.object_for(0xAAAA, 6), first);
    CY_CHECK_EQ(session.associations(), 2U);
    // A scene with nothing in it has no object to give, and says so rather than choosing zero.
    CY_CHECK_EQ(session.object_for(0xCCCC, 0), EditorSession::kNoObject);
}

CY_TEST_CASE("more identities than objects wrap rather than failing") {
    // An editor with more selected than the runtime holds is a legitimate state, and the
    // alternative — no gizmo at all — would be less informative than a gizmo on one of them.
    EditorSession session;
    for (u64 identity = 0; identity < 10; ++identity) {
        CY_CHECK(session.object_for(identity, 3) < 3U);
    }
    CY_CHECK_EQ(session.object_for(0, 3), 0U);
    CY_CHECK_EQ(session.object_for(3, 3), 0U);
}

CY_TEST_CASE("a move the editor committed is read out of its transaction") {
    cy::Array<TranslationDelta> deltas;
    const cy::Array<u8> transaction = a_move(0x5EED, 1.0F, 3.5F);
    const cy::Expected<u32, cy::Error> operations =
        read_translations(cy::Span<const u8>{transaction.data(), transaction.size()}, deltas);
    CY_REQUIRE(operations.has_value());
    CY_CHECK_EQ(*operations, 1U);
    CY_REQUIRE_EQ(deltas.size(), 1U);
    CY_CHECK_EQ(deltas[0].identity, 0x5EEDULL);
    CY_CHECK_NEAR(deltas[0].amount.x, 2.5F, 1e-5F);
    CY_CHECK_NEAR(deltas[0].amount.y, 0.0F, 1e-6F);
}

CY_TEST_CASE("a truncated transaction is refused at every length rather than half-applied") {
    // A transaction cut short at any point must not produce a partial move: the editor's document
    // has recorded the whole thing, and a runtime that applied half of it would drift from the
    // document with nothing to say about when.
    const cy::Array<u8> transaction = a_move(1, 0.0F, 1.0F);
    for (cy::usize length = 0; length + 1 < transaction.size(); ++length) {
        cy::Array<TranslationDelta> deltas;
        const cy::Expected<u32, cy::Error> read =
            read_translations(cy::Span<const u8>{transaction.data(), length}, deltas);
        CY_CHECK_FALSE(read.has_value());
    }
}

CY_TEST_CASE("an operation this runtime cannot apply is declined by name") {
    // A create or a delete needs the shared world M8's live editing brings. Declining is not a
    // failure of the editor's edit — the document has already recorded it — and the message says
    // which milestone answers it rather than leaving a reader to guess.
    cy::Array<u8> transaction;
    little_endian(transaction, 1, 8);
    little_endian(transaction, 1, 8);
    little_endian(transaction, 0, 8);
    text(transaction, "Delete entity");
    CY_REQUIRE(transaction.push_back(0));
    text(transaction, "designer");
    CY_REQUIRE(transaction.push_back(0));
    little_endian(transaction, 1, 4);
    CY_REQUIRE(transaction.push_back(1));  // Operation::DeleteNode
    little_endian(transaction, 5, 8);
    little_endian(transaction, 0, 8);

    cy::Array<TranslationDelta> deltas;
    const cy::Expected<u32, cy::Error> read =
        read_translations(cy::Span<const u8>{transaction.data(), transaction.size()}, deltas);
    CY_REQUIRE_FALSE(read.has_value());
    CY_CHECK(read.error().code == cy::ErrorCode::NotImplemented);
    CY_CHECK(deltas.empty());
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

CY_TEST_CASE("drawing outside the frame is clipped rather than corrupting memory") {
    // A handle whose object is at the edge of the viewport is drawn partly off it, and every
    // rasteriser in this file goes through one bounds-checked blend for exactly that reason.
    Frame frame(24, 24);
    cy::render::GizmoLayout layout;
    layout.frame_id = 1;
    layout.centre_x = 0.0F;
    layout.centre_y = 0.0F;
    layout.extent = 100.0F;
    CY_REQUIRE(layout.spots.push_back(
        cy::render::GizmoHandleSpot{cy::render::GizmoHandle::AxisX, -50.0F, -50.0F, 9.0F, 1.0F}));
    CY_REQUIRE(layout.spots.push_back(
        cy::render::GizmoHandleSpot{cy::render::GizmoHandle::AxisY, 400.0F, 400.0F, 9.0F, 1.0F}));
    draw_gizmo(frame.canvas(), layout, cy::render::GizmoHandle::Count);
    draw_selection_marker(frame.canvas(), -20.0F, 200.0F, 30.0F);
    // Reaching here without a sanitiser report is the assertion; the pixel is a witness that the
    // canvas was not simply ignored.
    CY_CHECK(frame.at(0, 0).x >= 0.0F);
}

CY_TEST_CASE("a null canvas is a no-op rather than a crash") {
    const Canvas nothing;
    draw_gizmo(nothing, a_layout(), cy::render::GizmoHandle::Count);
    draw_selection_marker(nothing, 1.0F, 1.0F, 4.0F);
    CY_CHECK(nothing.pixels == nullptr);
}
