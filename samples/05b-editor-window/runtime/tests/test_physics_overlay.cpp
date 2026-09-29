// SPDX-License-Identifier: MIT
// The physics debug layers and the joint gizmo, drawn into a frame with no device. Issue #29.
//
// The claim is the division `editor-viewport-and-gizmos` makes: the ENGINE draws what its physics
// world holds, where the frame's own view puts it, and draws only the layers the editor asked for.
// The reference backend is enough, because its `debug_draw` walks the same sink the Jolt one does.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/servers/render/picking.h>
#include <cy/test/test.h>

#include <cy_reflect_generated_scene.h>

#include "physics_overlay.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

using cy::f32;
using cy::i32;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::Vec3;
using namespace cy::sample::editor_window;
namespace physics = cy::physics;
namespace ser = cy::scene::serialization;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

constexpr u32 kWidth = 320;
constexpr u32 kHeight = 180;

constexpr cy::i32 kTolerance = 12;

/// An opaque black frame.
struct Frame {
    cy::Array<u8> pixels;

    Frame() {
        CY_REQUIRE(pixels.resize(static_cast<cy::usize>(kWidth) * kHeight * 4));
        for (cy::usize index = 0; index < pixels.size(); index += 4) {
            pixels[index] = 0;
            pixels[index + 1] = 0;
            pixels[index + 2] = 0;
            pixels[index + 3] = 0xFF;
        }
    }

    [[nodiscard]] Canvas canvas() noexcept { return Canvas{pixels.data(), kWidth, kHeight}; }

    [[nodiscard]] u32 lit() const noexcept {
        u32 count = 0;
        for (cy::usize index = 0; index < pixels.size(); index += 4) {
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
        return count;
    }

    /// Whether any pixel within `reach` of (x, y) is `colour` (0xRRGGBB) blended over the black
    /// frame: every channel the same fraction of the colour's, and that fraction at least a third.
    /// A one-pixel stroke on a half-pixel boundary is drawn at partial coverage, so an exact match
    /// would only find strokes that happen to land on pixel centres.
    [[nodiscard]] bool has_colour_near(f32 x, f32 y, u32 reach, u32 colour) const noexcept {
        const i32 want[3] = {static_cast<i32>((colour >> 16U) & 0xFFU),
                             static_cast<i32>((colour >> 8U) & 0xFFU),
                             static_cast<i32>(colour & 0xFFU)};
        const i32 brightest = std::max({want[0], want[1], want[2]});
        for (u32 dy = 0; dy <= reach * 2; ++dy) {
            for (u32 dx = 0; dx <= reach * 2; ++dx) {
                const auto px = static_cast<u32>(x) + dx - reach;
                const auto py = static_cast<u32>(y) + dy - reach;
                if (px >= kWidth || py >= kHeight) {
                    continue;
                }
                const cy::usize index = ((static_cast<cy::usize>(py) * kWidth) + px) * 4;
                const i32 have[3] = {pixels[index], pixels[index + 1], pixels[index + 2]};
                const i32 peak = std::max({have[0], have[1], have[2]});
                if (peak * 3 < brightest) {
                    continue;
                }
                bool matches = true;
                for (u32 channel = 0; channel < 3; ++channel) {
                    const i32 expected = (want[channel] * peak) / brightest;
                    matches = matches && std::abs(have[channel] - expected) <= kTolerance;
                }
                if (matches) {
                    return true;
                }
            }
        }
        return false;
    }
};

/// The frame's view: camera-relative, looking down −Z from `kEye`, as `view_of` in main.cpp builds.
constexpr Vec3 kEye{0.0F, 0.0F, 10.0F};

[[nodiscard]] cy::render::View frame_view() noexcept {
    cy::render::View view;
    view.desc.purpose = cy::render::ViewPurpose::EditorViewport;
    view.desc.viewport = cy::render::ViewportRect{0, 0, kWidth, kHeight};
    view.desc.projection.kind = cy::render::ProjectionKind::Perspective;
    view.desc.projection.fov_y_radians = 1.0471975512F;
    view.desc.projection.near_plane = 0.1F;
    view.desc.projection.far_plane = 0.0F;
    view.desc.camera = cy::Transform::identity();
    view.refresh();
    return view;
}

[[nodiscard]] cy::Vec2 pixel_of(const cy::render::View& view, Vec3 world) {
    cy::Vec2 pixel;
    CY_REQUIRE(cy::render::project_to_pixel(view, world - kEye, pixel));
    return pixel;
}

/// A reference solver with one static unit box at the origin.
class OneBox {
public:
    OneBox() noexcept {
        const auto made = physics::reference::create_server(allocator());
        if (!made || !(*made)->initialize()) {
            return;
        }
        server_ = *made;
        physics::WorldDescription description;
        description.body_capacity = 4;
        const auto world = server_->create_world(description);
        physics::ShapeDescription box;
        box.type = physics::ShapeType::Box;
        box.half_extents = Vec3{1.0F, 1.0F, 1.0F};
        const auto shape = server_->create_shape(box);
        if (!world || !shape) {
            return;
        }
        world_ = *world;
        physics::ColliderDescription collider;
        collider.shape = *shape;
        physics::BodyDescription body;
        body.motion = physics::MotionType::Static;
        body.colliders = &collider;
        body.collider_count = 1;
        ready_ = server_->create_body(world_, body).has_value();
    }

    ~OneBox() {
        if (server_ != nullptr) {
            server_->shutdown();
            physics::reference::destroy_server(server_, allocator());
        }
    }

    OneBox(const OneBox&) = delete;
    OneBox& operator=(const OneBox&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const physics::PhysicsServer& server() const noexcept { return *server_; }
    [[nodiscard]] physics::WorldHandle world() const noexcept { return world_; }

private:
    physics::PhysicsServer* server_ = nullptr;
    physics::WorldHandle world_;
    bool ready_ = false;
};

constexpr u32 kColliders = static_cast<u32>(physics::DebugDrawFlags::Colliders);

}  // namespace

CY_TEST_CASE("a collider is drawn where the frame's own view puts the simulated body") {
    OneBox scene;
    CY_REQUIRE(scene.ready());
    Frame frame;
    const cy::render::View view = frame_view();
    FrameDebugSink sink(frame.canvas(), view, kEye);
    CY_REQUIRE(draw_physics_overlays(sink, scene.server(), scene.world(), kColliders));
    // A box is its twelve edges.
    CY_CHECK_EQ(sink.drawn(), 12U);
    // The front face's top-right corner, in the static bodies' grey.
    const cy::Vec2 corner = pixel_of(view, Vec3{1.0F, 1.0F, 1.0F});
    CY_CHECK(
        frame.has_colour_near(corner.x, corner.y, 1, physics_colour(physics::DebugColor::Static)));
    // And nothing at the frame's corner, far from the box.
    CY_CHECK_FALSE(
        frame.has_colour_near(4.0F, 4.0F, 2, physics_colour(physics::DebugColor::Static)));
}

CY_TEST_CASE("only the layers the editor asked for are drawn, and none when it asked for none") {
    OneBox scene;
    CY_REQUIRE(scene.ready());
    const cy::render::View view = frame_view();

    Frame off;
    FrameDebugSink nothing(off.canvas(), view, kEye);
    CY_REQUIRE(draw_physics_overlays(nothing, scene.server(), scene.world(), 0U));
    CY_CHECK_EQ(nothing.drawn(), 0U);
    CY_CHECK_EQ(off.lit(), 0U);

    // Contacts on a world that has not stepped: asked for, and there are none to draw.
    Frame contacts;
    FrameDebugSink contact_sink(contacts.canvas(), view, kEye);
    CY_REQUIRE(draw_physics_overlays(contact_sink, scene.server(), scene.world(),
                                     static_cast<u32>(physics::DebugDrawFlags::Contacts)));
    CY_CHECK_EQ(contact_sink.drawn(), 0U);

    // A bit this build does not know is ignored rather than handed to the solver.
    Frame unknown;
    FrameDebugSink unknown_sink(unknown.canvas(), view, kEye);
    CY_REQUIRE(draw_physics_overlays(unknown_sink, scene.server(), scene.world(), 1U << 20U));
    CY_CHECK_EQ(unknown_sink.drawn(), 0U);
}

CY_TEST_CASE("a segment behind the camera is not folded onto the frame") {
    Frame frame;
    const cy::render::View view = frame_view();
    FrameDebugSink sink(frame.canvas(), view, kEye);
    sink.line(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 20.0F}, physics::DebugColor::Velocity);
    CY_CHECK_EQ(sink.drawn(), 0U);
    CY_CHECK_EQ(frame.lit(), 0U);
}

CY_TEST_CASE("the physics palette keeps the states a reader must tell apart distinct") {
    const physics::DebugColor states[] = {
        physics::DebugColor::Static,          physics::DebugColor::Kinematic,
        physics::DebugColor::DynamicAwake,    physics::DebugColor::DynamicAsleep,
        physics::DebugColor::Trigger,         physics::DebugColor::Contact,
        physics::DebugColor::ConstraintLimit,
    };
    for (const physics::DebugColor left : states) {
        for (const physics::DebugColor right : states) {
            if (left != right) {
                CY_CHECK(physics_colour(left) != physics_colour(right));
            }
        }
    }
}

namespace {

/// A door hinged to a frame one metre to its left: the gizmo's world.
constexpr std::string_view kHingedDoor =
    "cyworld 1\n"
    "type 1 runtime \"Transform\"\n"
    "  field 1 vec3 \"translation\" \"\"\n"
    "  field 2 quat \"rotation\" \"\"\n"
    "  field 3 vec3 \"scale\" \"\"\n"
    "type 2 runtime \"Joint\"\n"
    "  field 4 text \"kind\" \"\"\n"
    "  field 5 entity \"target\" \"\"\n"
    "  field 6 vec3 \"anchor\" \"\"\n"
    "  field 7 vec3 \"axis\" \"\"\n"
    "  field 8 float \"limit_min\" \"\"\n"
    "  field 9 float \"limit_max\" \"\"\n"
    "node 0 - \"default\" \"Frame\"\n"
    "  component 1\n"
    "    field 1 -1 0 0\n"
    "    field 2 0 0 0 1\n"
    "    field 3 1 1 1\n"
    "node 1 - \"default\" \"Door\"\n"
    "  component 1\n"
    "    field 1 1 0 0\n"
    "    field 2 0 0 0 1\n"
    "    field 3 2 2 2\n"
    "  component 2\n"
    "    field 4 \"hinge\"\n"
    "    field 5 0\n"
    "    field 6 -1 0 0\n"
    "    field 7 0 1 0\n"
    "    field 8 -1\n"
    "    field 9 1\n";

}  // namespace

CY_TEST_CASE("a selected hinge is drawn at its anchor with its axis and its limits") {
    // Resolved against the engine's schema, as the runtime's world is, so `Transform` is the
    // engine's `LocalTransform` and the door is where the file puts it.
    cy::reflect::TypeRegistry registry;
    CY_REQUIRE(cy::reflect::register_scene_types(registry));
    ser::AuthoringSchema schema(allocator());
    CY_REQUIRE(ser::build_authoring_schema(registry, schema));
    ser::World world(allocator());
    CY_REQUIRE(ser::read_world(kHingedDoor, "worlds/door.cyworld", world));
    CY_REQUIRE(ser::resolve_against(world, schema));
    Frame frame;
    const cy::render::View view = frame_view();
    FrameDebugSink sink(frame.canvas(), view, kEye);
    CY_REQUIRE(draw_authored_joint(sink, world, world.nodes()[1].identity));
    CY_CHECK(sink.drawn() > 0U);
    // The anchor is one metre left of the door in its UNSCALED frame, world (0, 0, 0), even though
    // the door is authored at scale two; the line to the joined frame runs from there to the
    // frame's origin at (-1, 0, 0).
    const cy::Vec2 joined = pixel_of(view, Vec3{-0.6F, 0.0F, 0.0F});
    CY_CHECK(frame.has_colour_near(joined.x, joined.y, 1,
                                   physics_colour(physics::DebugColor::Constraint)));
    // The authored axis is +Y, drawn up from the anchor in the limits' gold.
    const cy::Vec2 axis = pixel_of(view, Vec3{0.0F, 0.3F, 0.0F});
    CY_CHECK(frame.has_colour_near(axis.x, axis.y, 1,
                                   physics_colour(physics::DebugColor::ConstraintLimit)));

    // The frame node carries no joint, and nothing is drawn for it.
    Frame empty;
    FrameDebugSink none(empty.canvas(), view, kEye);
    CY_CHECK_FALSE(draw_authored_joint(none, world, world.nodes()[0].identity));
    CY_CHECK_EQ(empty.lit(), 0U);
}
