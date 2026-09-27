// SPDX-License-Identifier: MIT
// `integration.game_backend_camera` — ABI 1.3's `camera_*` entries over a real camera server.
// `add-swift-game-api`.
//
// OWNER: implementer A. Every case goes through `cy_get_interface`'s table with a `CameraAdapter`
// bound on a real `cy::abi::Host`, over a `CameraServer` evaluating a real rig: a target, a follow
// two metres up and six back, and a look-at. The primary view's viewport starts at (100, 50), so a
// projection that forgot the viewport origin would be off by exactly that.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/camera_backend.h>
#include <cy/servers/camera/server.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstring>

namespace {

using namespace cy::camera;
using cy::f32;
using cy::Name;
using cy::u32;
using cy::Vec3;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Engine);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

RigNodeDesc node(const char* id, const char* input, RigNodeKind kind) noexcept {
    RigNodeDesc desc;
    desc.id = Name::intern(id);
    desc.input = input == nullptr ? Name{} : Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// Target, follow at (0, 2, 6) from it with no smoothing, look at it with no smoothing.
RigDefinition follow_rig() noexcept {
    RigDefinition definition(allocator());
    definition.name = Name::intern("abi-follow");
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc follow = node("follow", "target", RigNodeKind::Follow);
    follow.follow.space = FollowSpace::World;
    follow.follow.offset = Vec3{0.0F, 2.0F, 6.0F};
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    RigNodeDesc look = node("look", "follow", RigNodeKind::LookAt);
    look.look_at.rotation_half_life = 0.0F;
    (void)definition.nodes.push_back(look);
    (void)definition.nodes.push_back(node("output", "look", RigNodeKind::Output));
    return definition;
}

struct Rig {
    CameraServer server{allocator()};
    cy::game_backend::CameraAdapter adapter{server, allocator()};
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    RigHandle rig;
    RigHandle other;

    [[nodiscard]] bool build() noexcept {
        if (!server.configure(CameraServerConfig{}) || !server.initialize()) {
            return false;
        }
        const RigDefinition definition = follow_rig();
        auto created = server.create_definition(definition);
        if (!created) {
            return false;
        }
        auto primary = server.create_rig(*created, RigConfig{});
        auto second = server.create_rig(*created, RigConfig{});
        if (!primary || !second) {
            return false;
        }
        rig = *primary;
        other = *second;
        RenderViewRequest request;
        request.viewport = cy::render::ViewportRect{100, 50, 1280, 720};
        adapter.set_primary_view(rig, request);
        cy::game_backend::bind(host, &adapter);
        return target_origin() && evaluate();
    }

    [[nodiscard]] bool target_origin() noexcept {
        TargetBinding binding;
        binding.kind = TargetKind::Position;
        return server.set_target(rig, binding).has_value();
    }

    [[nodiscard]] bool evaluate() noexcept {
        EvaluationContext context;
        context.delta_seconds = 1.0F / 60.0F;
        return server.evaluate(rig, context).has_value();
    }

    CyEngine engine() noexcept { return &host; }
    CyCamera camera() const noexcept { return rig.bits(); }
};

/// The same floats, element by element.
bool same(const f32* a, const f32* b, cy::usize count) noexcept {
    for (cy::usize index = 0; index < count; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

/// Every field of two views identical, without comparing padding.
bool identical(const CyCameraView& a, const CyCameraView& b) noexcept {
    return a.struct_size == b.struct_size && a.flags == b.flags &&
           same(a.pose.position, b.pose.position, 3U) &&
           same(a.pose.rotation, b.pose.rotation, 4U) && a.vertical_fov == b.vertical_fov &&
           a.ortho_height == b.ortho_height && a.near_plane == b.near_plane &&
           a.far_plane == b.far_plane && same(a.viewport, b.viewport, 4U);
}

f32 distance_to_line(const f32 origin[3], const f32 direction[3], Vec3 point) noexcept {
    const Vec3 o{origin[0], origin[1], origin[2]};
    const Vec3 d{direction[0], direction[1], direction[2]};
    const Vec3 to_point = point - o;
    const Vec3 along = d * dot(to_point, d);
    return length(to_point - along);
}

}  // namespace

CY_TEST_CASE("a world point projected to the screen and cast back lies on the pick ray") {
    Rig rig;
    CY_REQUIRE(rig.build());
    const cy::abi::game::PhaseScope frame(rig.host.game.clock, CY_PHASE_FRAME_UPDATE);

    CyCamera camera = CY_CAMERA_NULL;
    CY_REQUIRE_EQ(table().camera_active(rig.engine(), &camera), CY_RESULT_OK);
    CY_CHECK_EQ(camera, rig.camera());

    const Vec3 world{0.7F, 0.3F, -1.5F};
    const f32 points[3] = {world.x, world.y, world.z};
    CyScreenPoint screen{};
    CY_REQUIRE_EQ(table().camera_world_to_screen(rig.engine(), camera, points, 1, &screen),
                  CY_RESULT_OK);
    CY_CHECK_EQ(screen.flags, CY_SCREEN_POINT_ON_SCREEN);
    CY_CHECK_GT(screen.position[0], 100.0F + 640.0F);  // right of centre, inside the viewport
    CY_CHECK_LT(screen.position[1], 50.0F + 720.0F);
    CY_CHECK_GT(screen.depth, 0.0F);

    CyRay ray{};
    CY_REQUIRE_EQ(table().camera_screen_to_ray(rig.engine(), camera, screen.position, &ray),
                  CY_RESULT_OK);
    CY_CHECK_LT(distance_to_line(ray.origin, ray.direction, world), 1e-3F);

    // The ray starts on the near plane, in front of the eye, and reaches the far plane.
    CyCameraView view{};
    CY_REQUIRE_EQ(table().camera_view(rig.engine(), camera, &view), CY_RESULT_OK);
    const Vec3 eye{view.pose.position[0], view.pose.position[1], view.pose.position[2]};
    CY_CHECK_NEAR(eye.y, 2.0F, 1e-4F);
    CY_CHECK_NEAR(eye.z, 6.0F, 1e-4F);
    const Vec3 origin{ray.origin[0], ray.origin[1], ray.origin[2]};
    const Vec3 forward = normalize(Vec3{0.0F, 0.0F, 0.0F} - eye);
    CY_CHECK_NEAR(dot(origin - eye, forward), view.near_plane, 1e-4F);
    CY_CHECK_GT(ray.max_distance, 1000.0F);  // an infinite far plane reaches the stand-in far
}

CY_TEST_CASE("the view reports the evaluated pose, the lens and the viewport") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyCameraView view{};
    view.struct_size = sizeof(view);
    CY_REQUIRE_EQ(table().camera_view(rig.engine(), rig.camera(), &view), CY_RESULT_OK);
    CY_CHECK_EQ(view.flags, 0U);
    CY_CHECK_GT(view.vertical_fov, 0.0F);
    CY_CHECK_EQ(view.ortho_height, 0.0F);
    CY_CHECK_GT(view.near_plane, 0.0F);
    CY_CHECK_EQ(view.viewport[0], 100.0F);
    CY_CHECK_EQ(view.viewport[1], 50.0F);
    CY_CHECK_EQ(view.viewport[2], 1280.0F);
    CY_CHECK_EQ(view.viewport[3], 720.0F);

    // Asked twice, answered the same, byte for byte.
    CyCameraView again{};
    again.struct_size = sizeof(again);
    CY_REQUIRE_EQ(table().camera_view(rig.engine(), rig.camera(), &again), CY_RESULT_OK);
    CY_CHECK(identical(view, again));
}

CY_TEST_CASE("a point behind the camera is flagged, never reported on screen") {
    Rig rig;
    CY_REQUIRE(rig.build());
    const f32 points[6] = {0.0F, 2.0F, 20.0F, 0.0F, 0.0F, 0.0F};
    CyScreenPoint screen[2] = {};
    CY_REQUIRE_EQ(table().camera_world_to_screen(rig.engine(), rig.camera(), points, 2, screen),
                  CY_RESULT_OK);
    CY_CHECK_EQ(screen[0].flags, CY_SCREEN_POINT_BEHIND);
    CY_CHECK_LT(screen[0].depth, 0.0F);
    CY_CHECK_EQ(screen[1].flags, CY_SCREEN_POINT_ON_SCREEN);
    // The framed point is the centre of the viewport.
    CY_CHECK_NEAR(screen[1].position[0], 100.0F + 640.0F, 1e-2F);
    CY_CHECK_NEAR(screen[1].position[1], 50.0F + 360.0F, 1e-2F);
}

CY_TEST_CASE("a resimulated camera_set_target has no effect; a live one binds, cuts and frames") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyCameraTarget target{};
    target.struct_size = sizeof(target);
    target.position[0] = 50.0F;
    target.yaw = 0.25F;
    target.pitch = -0.9F;
    target.distance = 30.0F;
    target.blend_seconds = 0.0F;

    const u32 cuts_before = rig.server.evaluated(rig.rig)->cut_epoch;
    {
        const cy::abi::game::PhaseScope fixed(rig.host.game.clock, CY_PHASE_FIXED_UPDATE);
        rig.host.game.clock.flags = CY_TIME_RESIMULATING;
        CY_CHECK_EQ(table().camera_set_target(rig.engine(), rig.camera(), &target), CY_RESULT_OK);
        rig.host.game.clock.flags = 0;
    }
    CY_CHECK_EQ(rig.server.target(rig.rig)->position.x, 0.0F);
    CY_CHECK(rig.adapter.framing(rig.rig) == nullptr);
    CY_REQUIRE(rig.evaluate());
    CY_CHECK_EQ(rig.server.evaluated(rig.rig)->cut_epoch, cuts_before);
    CY_CHECK_NEAR(rig.server.evaluated(rig.rig)->pose.translation.x, 0.0F, 1e-4F);

    {
        const cy::abi::game::PhaseScope fixed(rig.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(table().camera_set_target(rig.engine(), rig.camera(), &target), CY_RESULT_OK);
    }
    CY_CHECK_EQ(rig.server.target(rig.rig)->kind, TargetKind::Position);
    CY_CHECK_EQ(rig.server.target(rig.rig)->position.x, 50.0F);
    const auto* framing = rig.adapter.framing(rig.rig);
    CY_REQUIRE(framing != nullptr);
    CY_CHECK_EQ(framing->yaw, 0.25F);
    CY_CHECK_EQ(framing->pitch, -0.9F);
    CY_CHECK_EQ(framing->distance, 30.0F);
    CY_CHECK_EQ(framing->revision, 1U);
    CY_REQUIRE(rig.evaluate());
    CY_CHECK_EQ(rig.server.evaluated(rig.rig)->cut_epoch, cuts_before + 1U);  // zero blend: a cut
    CY_CHECK_NEAR(rig.server.evaluated(rig.rig)->pose.translation.x, 50.0F, 1e-3F);

    // Following an entity binds it by its `CyEntity` value.
    target.flags = CY_CAMERA_TARGET_FOLLOW_ENTITY;
    target.entity = 0x200000011ULL;
    target.blend_seconds = 0.5F;
    CY_CHECK_EQ(table().camera_set_target(rig.engine(), rig.camera(), &target), CY_RESULT_OK);
    CY_CHECK_EQ(rig.server.target(rig.rig)->kind, TargetKind::Entity);
    CY_CHECK_EQ(rig.server.target(rig.rig)->stable_id, 0x200000011ULL);
    CY_CHECK_EQ(rig.adapter.framing(rig.rig)->revision, 2U);

    target.distance = -1.0F;
    CY_CHECK_EQ(table().camera_set_target(rig.engine(), rig.camera(), &target),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a pose override replaces the rig until it is cleared") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyPose pose{};  // an all-zero quaternion: the identity
    pose.position[0] = 3.0F;
    pose.position[1] = 40.0F;
    CY_CHECK_EQ(table().camera_set_pose(rig.engine(), rig.camera(), &pose), CY_RESULT_OK);
    CY_REQUIRE(rig.evaluate());
    const EvaluatedCamera* evaluated = rig.server.evaluated(rig.rig);
    CY_CHECK(evaluated->pose_overridden);
    CY_CHECK_NEAR(evaluated->pose.translation.y, 40.0F, 1e-4F);
    CY_CHECK_NEAR(evaluated->pose.rotation.w, 1.0F, 1e-6F);

    CY_CHECK_EQ(table().camera_clear_pose(rig.engine(), rig.camera()), CY_RESULT_OK);
    CY_REQUIRE(rig.evaluate());
    CY_CHECK_FALSE(rig.server.evaluated(rig.rig)->pose_overridden);
    CY_CHECK_NEAR(rig.server.evaluated(rig.rig)->pose.translation.y, 2.0F, 1e-4F);

    const f32 nan = std::nanf("");
    pose.position[0] = nan;
    CY_CHECK_EQ(table().camera_set_pose(rig.engine(), rig.camera(), &pose),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a destroyed rig is NOT_FOUND, and without a primary view there is no camera") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyCameraView view{};
    // A live rig that is not the primary view has no viewport to project with.
    CY_CHECK_EQ(table().camera_view(rig.engine(), rig.other.bits(), &view), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().camera_clear_pose(rig.engine(), rig.other.bits()), CY_RESULT_OK);

    const CyCamera stale = rig.camera();
    rig.server.destroy_rig(rig.rig);
    CyCamera camera = 9;
    CY_CHECK_EQ(table().camera_active(rig.engine(), &camera), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(camera, 9U);
    CY_CHECK_EQ(table().camera_view(rig.engine(), stale, &view), CY_RESULT_NOT_FOUND);
    CyPose pose{};
    CY_CHECK_EQ(table().camera_set_pose(rig.engine(), stale, &pose), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().camera_clear_pose(rig.engine(), CY_CAMERA_NULL), CY_RESULT_NOT_FOUND);

    rig.adapter.clear_primary_view();
    CY_CHECK_EQ(table().camera_active(rig.engine(), &camera), CY_RESULT_UNAVAILABLE);
}
