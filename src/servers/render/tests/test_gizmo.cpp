// Engine-side gizmo geometry. M7 task 5b.3, `editor-viewport-and-gizmos`.
//
// Three requirements are checked here, and the second is the one a screenshot cannot answer:
//
//   * "Gizmo geometry generation, depth handling, and screen-constant sizing SHALL be produced by
//     the engine" — so there is a function that produces handle positions, and it is this one.
//   * **Screen-constant sizing.** A gizmo drawn in world units becomes unusable exactly when
//     precision matters most and the failure is gradual enough that nobody files it. The case
//     below pulls the camera away by a factor of a hundred and asserts that the extent and every
//     handle's distance from the centre are unchanged.
//   * The three concentric centre affordances stay separately targetable, which is a property of
//     the DEPTHS this file publishes rather than of the editor's hit test — the editor orders by
//     distance and then by depth, and at the exact centre every distance is zero.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/gizmo.h>
#include <cy/servers/render/picking.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::u8;
using namespace cy::render;

namespace {

/// A view looking down −Z from `distance`, sized like an editor viewport.
[[nodiscard]] View viewport_at(f32 distance) noexcept {
    View view;
    view.desc.purpose = ViewPurpose::EditorViewport;
    view.desc.viewport = ViewportRect{0, 0, 1280, 720};
    view.desc.camera = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, distance});
    view.desc.projection.fov_y_radians = 1.0471975512F;
    view.refresh();
    return view;
}

[[nodiscard]] f32 reach_of(const GizmoLayout& layout, GizmoHandle handle) noexcept {
    const GizmoHandleSpot* spot = layout.spot(handle);
    if (spot == nullptr) {
        return -1.0F;
    }
    const f32 dx = spot->x - layout.centre_x;
    const f32 dy = spot->y - layout.centre_y;
    return std::sqrt((dx * dx) + (dy * dy));
}

/// The editor's own encoding of a `cy_editor_services::gizmo::Request`, written by hand so that
/// the decoder is checked against the wire rather than against this file's own encoder — of which
/// there deliberately is none, because the engine never sends an intent.
[[nodiscard]] cy::Array<u8> encoded_intent(u64 frame, u8 mode, u8 space, u8 pivot,
                                           std::initializer_list<u64> identities) {
    cy::Array<u8> bytes;
    auto little_endian = [&bytes](u64 value, cy::usize width) {
        for (cy::usize index = 0; index < width; ++index) {
            CY_REQUIRE(bytes.push_back(static_cast<u8>((value >> (index * 8)) & 0xFFU)));
        }
    };
    little_endian(frame, 8);
    CY_REQUIRE(bytes.push_back(mode));
    CY_REQUIRE(bytes.push_back(space));
    CY_REQUIRE(bytes.push_back(pivot));
    little_endian(identities.size(), 4);
    for (const u64 identity : identities) {
        little_endian(identity, 8);
    }
    return bytes;
}

}  // namespace

CY_TEST_CASE("the gizmo is the same size on screen however far away the camera is") {
    // THE REQUIREMENT THIS FILE EXISTS FOR. `editor-viewport-and-gizmos` requires screen-constant
    // sizing, and `cy_editor_viewport::layout::screen_constant` refuses a runtime that gets it
    // wrong — but only after the fact, over a run of frames a person had to produce. This is the
    // same property, one function call away from where it is decided.
    const GizmoStyle style;
    GizmoLayout near = build_gizmo_layout(viewport_at(2.0F), cy::Vec3{0.0F, 0.0F, 0.0F},
                                          cy::Quat::identity(), GizmoMode::Universal, 11, style);
    GizmoLayout far = build_gizmo_layout(viewport_at(200.0F), cy::Vec3{0.0F, 0.0F, 0.0F},
                                         cy::Quat::identity(), GizmoMode::Universal, 12, style);

    CY_REQUIRE_FALSE(near.empty());
    CY_REQUIRE_FALSE(far.empty());
    CY_CHECK_NEAR(near.extent, style.extent_pixels, 1e-3F);
    CY_CHECK_NEAR(far.extent, near.extent, 1e-3F);

    // The extent is a declared number, so it would stay constant even if the handles did not. The
    // check that matters is the DRAWN one: the X arrow is the same number of pixels from the
    // centre at two metres and at two hundred.
    CY_CHECK_NEAR(reach_of(near, GizmoHandle::AxisX), style.extent_pixels, 0.5F);
    CY_CHECK_NEAR(reach_of(far, GizmoHandle::AxisX), reach_of(near, GizmoHandle::AxisX), 0.5F);
    CY_CHECK_NEAR(reach_of(far, GizmoHandle::BoxY), reach_of(near, GizmoHandle::BoxY), 0.5F);
    CY_CHECK_NEAR(reach_of(far, GizmoHandle::PlaneXY), reach_of(near, GizmoHandle::PlaneXY), 0.5F);
}

CY_TEST_CASE("a world-sized gizmo would have been caught: the pixel scale falls with distance") {
    // The counterfactual, so the case above is checking something. `world_per_pixel` is what makes
    // the gizmo screen-constant, and it is inversely proportional to distance — which is exactly
    // the halving-per-doubling a world-space gizmo would show on screen.
    const f32 near = world_per_pixel(viewport_at(2.0F), cy::Vec3{0.0F, 0.0F, 0.0F});
    const f32 far = world_per_pixel(viewport_at(200.0F), cy::Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK(near > 0.0F);
    CY_CHECK_NEAR(far / near, 100.0F, 0.5F);
}

CY_TEST_CASE("an orthographic view sizes the gizmo from its own height, not from a depth") {
    View view = viewport_at(5.0F);
    view.desc.projection.kind = ProjectionKind::Orthographic;
    view.desc.projection.ortho_height = 7.2F;
    view.refresh();
    // 7.2 world units over 720 pixels is exactly one centimetre a pixel, at every depth.
    CY_CHECK_NEAR(world_per_pixel(view, cy::Vec3{0.0F, 0.0F, 0.0F}), 0.01F, 1e-6F);
    CY_CHECK_NEAR(world_per_pixel(view, cy::Vec3{0.0F, 0.0F, -50.0F}), 0.01F, 1e-6F);
}

CY_TEST_CASE("each mode draws the forms the editor's visual language says it draws") {
    const View view = viewport_at(6.0F);
    const cy::Vec3 pivot{0.0F, 0.0F, 0.0F};

    const GizmoLayout move =
        build_gizmo_layout(view, pivot, cy::Quat::identity(), GizmoMode::Translate, 1);
    CY_CHECK(move.spot(GizmoHandle::AxisX) != nullptr);
    CY_CHECK(move.spot(GizmoHandle::PlaneXY) != nullptr);
    CY_CHECK(move.spot(GizmoHandle::Screen) != nullptr);
    CY_CHECK(move.spot(GizmoHandle::RingX) == nullptr);
    CY_CHECK(move.spot(GizmoHandle::BoxX) == nullptr);

    const GizmoLayout rotate =
        build_gizmo_layout(view, pivot, cy::Quat::identity(), GizmoMode::Rotate, 2);
    CY_CHECK(rotate.spot(GizmoHandle::RingZ) != nullptr);
    CY_CHECK(rotate.spot(GizmoHandle::ScreenRing) != nullptr);
    CY_CHECK(rotate.spot(GizmoHandle::AxisX) == nullptr);

    const GizmoLayout scale =
        build_gizmo_layout(view, pivot, cy::Quat::identity(), GizmoMode::Scale, 3);
    CY_CHECK(scale.spot(GizmoHandle::BoxZ) != nullptr);
    CY_CHECK(scale.spot(GizmoHandle::Uniform) != nullptr);
    CY_CHECK(scale.spot(GizmoHandle::AxisX) == nullptr);

    const GizmoLayout universal =
        build_gizmo_layout(view, pivot, cy::Quat::identity(), GizmoMode::Universal, 4);
    for (const GizmoHandle handle :
         {GizmoHandle::AxisX, GizmoHandle::PlaneYZ, GizmoHandle::RingY, GizmoHandle::BoxX,
          GizmoHandle::Screen, GizmoHandle::Uniform, GizmoHandle::ScreenRing}) {
        CY_CHECK(universal.spot(handle) != nullptr);
    }
}

CY_TEST_CASE("the centre's three affordances are published at three depths, nearest last") {
    // The editor resolves a click by distance and then by depth, and at the exact centre every
    // distance is zero — so if these three shared a depth the LIST ORDER would decide which handle
    // a click on the centre grabbed. That is the defect this bias exists to remove.
    const GizmoLayout layout = build_gizmo_layout(viewport_at(6.0F), cy::Vec3{0.0F, 0.0F, 0.0F},
                                                  cy::Quat::identity(), GizmoMode::Universal, 5);
    const GizmoHandleSpot* ring = layout.spot(GizmoHandle::ScreenRing);
    const GizmoHandleSpot* circle = layout.spot(GizmoHandle::Screen);
    const GizmoHandleSpot* cube = layout.spot(GizmoHandle::Uniform);
    CY_REQUIRE(ring != nullptr);
    CY_REQUIRE(circle != nullptr);
    CY_REQUIRE(cube != nullptr);
    CY_CHECK(cube->depth < circle->depth);
    CY_CHECK(circle->depth < ring->depth);
    CY_CHECK_NEAR(cube->x, layout.centre_x, 1e-4F);
    CY_CHECK_NEAR(circle->y, layout.centre_y, 1e-4F);
    CY_CHECK(cube->radius < circle->radius);
    CY_CHECK(circle->radius < ring->radius);
}

CY_TEST_CASE("a pivot behind the camera draws no gizmo rather than one at the edge") {
    // A clamped handle is worse than a missing one: it is drawn where the geometry is not, and a
    // click on it manipulates an axis pointing away from the user.
    const GizmoLayout behind = build_gizmo_layout(viewport_at(6.0F), cy::Vec3{0.0F, 0.0F, 40.0F},
                                                  cy::Quat::identity(), GizmoMode::Universal, 6);
    CY_CHECK(behind.empty());
    CY_CHECK_EQ(behind.frame_id, 6ULL);
    CY_CHECK_NEAR(behind.extent, 0.0F, 1e-6F);
}

CY_TEST_CASE("a view with no viewport draws nothing rather than an infinite gizmo") {
    View view = viewport_at(6.0F);
    view.desc.viewport = ViewportRect{0, 0, 0, 0};
    view.refresh();
    CY_CHECK(build_gizmo_layout(view, cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Quat::identity(),
                                GizmoMode::Translate, 7)
                 .empty());
}

CY_TEST_CASE("local space turns the gizmo with the object and world space does not") {
    const View view = viewport_at(6.0F);
    const cy::Vec3 pivot{0.0F, 0.0F, 0.0F};
    // A quarter turn about Y takes the object's +X onto world −Z, which points at the camera and
    // therefore projects near the centre rather than out to the right.
    const cy::Quat turned =
        cy::Quat::from_axis_angle(cy::Vec3{0.0F, 1.0F, 0.0F}, cy::math::kPi * 0.5F);
    const GizmoLayout world =
        build_gizmo_layout(view, pivot, cy::Quat::identity(), GizmoMode::Translate, 8);
    const GizmoLayout local = build_gizmo_layout(view, pivot, turned, GizmoMode::Translate, 9);
    CY_CHECK(reach_of(world, GizmoHandle::AxisX) > 80.0F);
    CY_CHECK(reach_of(local, GizmoHandle::AxisX) < 2.0F);
    CY_CHECK_NEAR(reach_of(local, GizmoHandle::AxisY), reach_of(world, GizmoHandle::AxisY), 0.5F);
}

CY_TEST_CASE("a gizmo intent decodes exactly what the editor encoded") {
    GizmoIntent intent;
    const cy::Array<u8> bytes = encoded_intent(1016, 1, 1, 2, {7, 9, 0xFFFF'FFFF'FFFFULL});
    CY_REQUIRE(decode_gizmo_intent(cy::Span<const u8>{bytes.data(), bytes.size()}, intent));
    CY_CHECK_EQ(intent.frame_id, 1016ULL);
    CY_CHECK(intent.mode == GizmoMode::Rotate);
    CY_CHECK(intent.space == GizmoSpace::Local);
    CY_CHECK(intent.pivot == GizmoPivot::Bounds);
    CY_REQUIRE_EQ(intent.identities.size(), 3U);
    CY_CHECK_EQ(intent.identities[2], 0xFFFF'FFFF'FFFFULL);
}

CY_TEST_CASE("an empty selection is a request and not an absence") {
    // "Draw nothing" has to be decodable, or a selection that became empty leaves the last gizmo on
    // the screen for ever.
    GizmoIntent intent;
    const cy::Array<u8> bytes = encoded_intent(4, 0, 0, 0, {});
    CY_REQUIRE(decode_gizmo_intent(cy::Span<const u8>{bytes.data(), bytes.size()}, intent));
    CY_CHECK(intent.identities.empty());
    CY_CHECK_EQ(intent.frame_id, 4ULL);
}

CY_TEST_CASE("a truncated or unknown intent is refused rather than half-read") {
    GizmoIntent intent;
    cy::Array<u8> bytes = encoded_intent(1, 0, 0, 0, {5});
    for (cy::usize length = 0; length < bytes.size(); ++length) {
        CY_CHECK_FALSE(decode_gizmo_intent(cy::Span<const u8>{bytes.data(), length}, intent));
    }
    // A count that claims more identities than the message holds is a truncated message or a
    // different protocol; reserving for it would be an allocation sized by a peer.
    cy::Array<u8> lying = encoded_intent(1, 0, 0, 0, {5});
    lying[10] = 0xFFU;
    lying[11] = 0xFFU;
    CY_CHECK_FALSE(decode_gizmo_intent(cy::Span<const u8>{lying.data(), lying.size()}, intent));

    cy::Array<u8> unknown_mode = encoded_intent(1, 9, 0, 0, {});
    CY_CHECK_FALSE(
        decode_gizmo_intent(cy::Span<const u8>{unknown_mode.data(), unknown_mode.size()}, intent));
    cy::Array<u8> unknown_space = encoded_intent(1, 0, 9, 0, {});
    CY_CHECK_FALSE(decode_gizmo_intent(
        cy::Span<const u8>{unknown_space.data(), unknown_space.size()}, intent));
}

CY_TEST_CASE("a custom space carries its four lanes after the tag") {
    cy::Array<u8> bytes;
    auto little_endian = [&bytes](u64 value, cy::usize width) {
        for (cy::usize index = 0; index < width; ++index) {
            CY_REQUIRE(bytes.push_back(static_cast<u8>((value >> (index * 8)) & 0xFFU)));
        }
    };
    auto float_bits = [&little_endian](f32 value) {
        cy::u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        little_endian(bits, 4);
    };
    little_endian(21, 8);
    CY_REQUIRE(bytes.push_back(0));  // translate
    CY_REQUIRE(bytes.push_back(4));  // custom
    float_bits(0.0F);
    float_bits(0.7071068F);
    float_bits(0.0F);
    float_bits(0.7071068F);
    CY_REQUIRE(bytes.push_back(0));  // pivot
    little_endian(0, 4);

    GizmoIntent intent;
    CY_REQUIRE(decode_gizmo_intent(cy::Span<const u8>{bytes.data(), bytes.size()}, intent));
    CY_CHECK(intent.space == GizmoSpace::Custom);
    CY_CHECK_NEAR(intent.custom_space.y, 0.7071068F, 1e-6F);

    const View view = viewport_at(6.0F);
    CY_CHECK_NEAR(gizmo_axes(intent, view, cy::Quat::identity()).y, 0.7071068F, 1e-6F);
}

CY_TEST_CASE("an encoded layout is the bytes the editor's decoder reads") {
    // The wire, byte for byte: a version, the frame, the mode, three floats, a count, and five
    // fields per handle. `cy_test_integration_editor_gizmo_wire` is what checks this against the
    // editor's own decoder; this case checks the sizes and the first few fields, so that a change
    // to the layout fails here rather than two languages away.
    GizmoLayout layout;
    layout.frame_id = 0x0102'0304'0506'0708ULL;
    layout.mode = GizmoMode::Scale;
    layout.centre_x = 640.0F;
    layout.centre_y = 360.0F;
    layout.extent = 88.0F;
    CY_REQUIRE(
        layout.spots.push_back(GizmoHandleSpot{GizmoHandle::Uniform, 640.0F, 360.0F, 9.0F, 6.0F}));
    cy::Array<u8> bytes;
    CY_REQUIRE(encode_gizmo_layout(layout, bytes));
    CY_REQUIRE_EQ(bytes.size(), 1U + 8U + 1U + 12U + 4U + (1U + 16U));
    CY_CHECK_EQ(bytes[0], 1U);
    CY_CHECK_EQ(bytes[1], 0x08U);
    CY_CHECK_EQ(bytes[8], 0x01U);
    CY_CHECK_EQ(bytes[9], static_cast<u8>(GizmoMode::Scale));
    CY_CHECK_EQ(bytes[22], 1U);  // the spot count's low byte, after the three floats
    CY_CHECK_EQ(bytes[26], static_cast<u8>(GizmoHandle::Uniform));
}

CY_TEST_CASE("the handle codes are the indices the editor's Handle::ALL declares") {
    // The one thing about this wire a compiler cannot check: the editor writes a handle's INDEX in
    // its own enumeration, and the two enumerations are in two languages. Restating the expected
    // codes here means a reordering on either side fails a test rather than moving every handle by
    // one and turning a click on the X arrow into a drag of the Y one.
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::AxisX), 0U);
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::PlaneXY), 3U);
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::RingX), 6U);
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::ScreenRing), 9U);
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::BoxX), 10U);
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::Screen), 13U);
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::Uniform), 14U);
    CY_CHECK_EQ(static_cast<u8>(GizmoHandle::Count), 15U);
}

// --- The cross-language fixture ---------------------------------------------------------------
//
// THE ONE THING NEITHER COMPILER CAN CHECK. `encode_gizmo_layout` writes what
// `cy_editor_viewport::layout::GizmoLayout::decode` reads, and the two are in two languages with no
// shared declaration between them. A comment saying they agree is worth nothing; a restatement in
// each is two things that drift.
//
// So there is ONE artefact: `data/gizmo_layout_v1.wire`, the bytes this encoder produces for the
// layout below. This case writes it when `CY_UPDATE_GIZMO_WIRE` is set and compares against it
// otherwise; `editor/crates/cy-editor-services/tests/the_engines_gizmo_layout_decodes.rs` reads the
// same file with the editor's own decoder and asserts the values back. A change to either side
// fails one of the two, and the file names which.

namespace {

/// The fixture layout. Deliberately one of every kind of handle, at values that are exact in f32 —
/// a fixture with a rounding difference in it would fail for a reason nobody could act on.
[[nodiscard]] GizmoLayout fixture_layout() {
    GizmoLayout layout;
    layout.frame_id = 1016;
    layout.mode = GizmoMode::Universal;
    layout.centre_x = 640.0F;
    layout.centre_y = 360.0F;
    layout.extent = 88.0F;
    const GizmoHandleSpot spots[] = {
        {GizmoHandle::AxisX, 728.0F, 360.0F, 7.0F, 6.0F},
        {GizmoHandle::AxisY, 640.0F, 272.0F, 7.0F, 6.0F},
        {GizmoHandle::PlaneXY, 673.5F, 326.5F, 9.0F, 6.0F},
        {GizmoHandle::RingZ, 640.0F, 448.0F, 5.0F, 5.5F},
        {GizmoHandle::BoxX, 703.375F, 360.0F, 8.0F, 6.0F},
        {GizmoHandle::ScreenRing, 640.0F, 360.0F, 103.84F, 6.0F},
        {GizmoHandle::Screen, 640.0F, 360.0F, 15.0F, 5.98F},
        {GizmoHandle::Uniform, 640.0F, 360.0F, 9.0F, 5.96F},
    };
    for (const GizmoHandleSpot& spot : spots) {
        CY_REQUIRE(layout.spots.push_back(spot));
    }
    return layout;
}

}  // namespace

CY_TEST_CASE("the encoded layout matches the fixture the editor's decoder is checked against") {
    cy::Array<u8> bytes;
    CY_REQUIRE(encode_gizmo_layout(fixture_layout(), bytes));

    const char* path = CY_GIZMO_WIRE_FIXTURE;
    if (std::getenv("CY_UPDATE_GIZMO_WIRE") != nullptr) {
        std::FILE* file = std::fopen(path, "wb");
        CY_REQUIRE(file != nullptr);
        CY_CHECK_EQ(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
        (void)std::fclose(file);
        CY_TEST_MESSAGE("rewrote " << path << "; run the editor's suite to check the other side");
        return;
    }

    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        CY_TEST_MESSAGE("the cross-language fixture " << path
                                                      << " is missing; regenerate it with "
                                                         "CY_UPDATE_GIZMO_WIRE=1");
    }
    CY_REQUIRE(file != nullptr);
    cy::Array<u8> committed;
    CY_REQUIRE(committed.resize(bytes.size() + 1));
    const cy::usize read = std::fread(committed.data(), 1, committed.size(), file);
    (void)std::fclose(file);
    CY_REQUIRE_EQ(read, bytes.size());
    for (cy::usize index = 0; index < bytes.size(); ++index) {
        CY_CHECK_EQ(static_cast<u32>(committed[index]), static_cast<u32>(bytes[index]));
    }
}
