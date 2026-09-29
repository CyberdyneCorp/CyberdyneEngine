// SPDX-License-Identifier: MIT
// Physics debug layers and joint gizmos, drawn into the published frame. See physics_overlay.h.

#include "physics_overlay.h"

#include <cy/gameplay/play/joints.h>
#include <cy/servers/render/picking.h>

namespace cy::sample::editor_window {
namespace {

namespace ser = scene::serialization;

/// Where a node is authored, without scale: a body has none, and a joint frame is in the body's.
[[nodiscard]] Transform unscaled_placement(const ser::World& world,
                                           const ser::WorldNode& node) noexcept {
    Transform placement;
    (void)ser::transform_of(world, node, placement);
    placement.scale = Vec3{1.0F, 1.0F, 1.0F};
    return placement;
}

/// Whether a kind has an axis an author sets: everything but the three that join at a point.
[[nodiscard]] bool has_axis(physics::ConstraintType type) noexcept {
    return type != physics::ConstraintType::Fixed && type != physics::ConstraintType::Point &&
           type != physics::ConstraintType::Distance;
}

/// How far the authored axis is drawn from anchor A, in metres.
constexpr f32 kAxisLength = 0.35F;

}  // namespace

u32 physics_colour(physics::DebugColor color) noexcept {
    switch (color) {
        case physics::DebugColor::Static:
            return 0xA2'AB'B4U;
        case physics::DebugColor::Kinematic:
        case physics::DebugColor::QueryShape:
            return 0x4C'9A'FFU;
        case physics::DebugColor::DynamicAwake:
            return 0x35'C0'7CU;
        case physics::DebugColor::DynamicAsleep:
            return 0x5E'6B'66U;
        case physics::DebugColor::Trigger:
            return 0xF0'91'3AU;
        case physics::DebugColor::Contact:
        case physics::DebugColor::QueryResult:
            return 0xFF'6B'60U;
        case physics::DebugColor::Normal:
        case physics::DebugColor::Constraint:
            return 0xE6'E9'ECU;
        case physics::DebugColor::Velocity:
            return 0x4C'C2'D6U;
        case physics::DebugColor::Bounds:
            return 0x5A'62'6AU;
        case physics::DebugColor::ConstraintLimit:
            return 0xE5'B9'5CU;
    }
    return 0xE6'E9'ECU;
}

void FrameDebugSink::line(Vec3 from, Vec3 to, physics::DebugColor color) noexcept {
    Vec2 start;
    Vec2 end;
    if (!render::project_to_pixel(view_, from - eye_, start) ||
        !render::project_to_pixel(view_, to - eye_, end)) {
        return;
    }
    draw_thin_line(canvas_, start.x, start.y, end.x, end.y, physics_colour(color));
    ++drawn_;
}

void FrameDebugSink::contact(Vec3 position, Vec3 normal, f32 penetration) noexcept {
    // The base sink's stub along the normal, plus a small cross at the point itself, because a
    // resting contact has no penetration and its normal alone reads as one more collider edge.
    physics::DebugDrawSink::contact(position, normal, penetration);
    constexpr f32 kArm = 0.04F;
    line(position - Vec3{kArm, 0.0F, 0.0F}, position + Vec3{kArm, 0.0F, 0.0F},
         physics::DebugColor::Contact);
    line(position - Vec3{0.0F, 0.0F, kArm}, position + Vec3{0.0F, 0.0F, kArm},
         physics::DebugColor::Contact);
}

Status draw_physics_overlays(FrameDebugSink& sink, const physics::PhysicsServer& server,
                             physics::WorldHandle world, u32 flags) noexcept {
    const u32 known = flags & static_cast<u32>(physics::DebugDrawFlags::All);
    if (known == 0U) {
        return ok();
    }
    return server.debug_draw(world, static_cast<physics::DebugDrawFlags>(known), sink);
}

bool draw_authored_joint(FrameDebugSink& sink, const ser::World& world, u64 identity) noexcept {
    const u32 index = world.index_of(identity);
    if (index == ser::WorldNode::kNoParent) {
        return false;
    }
    const Expected<gameplay::AuthoredJoint, Error> joint =
        gameplay::authored_joint(world, world.nodes()[index]);
    if (!joint) {
        return false;
    }
    const Transform body_a = unscaled_placement(world, world.nodes()[index]);
    const physics::ConstraintDescription& description = joint->description;
    const Transform anchor_a = body_a * description.frame_a;
    // Frame B is derived exactly as play derives it, so the gizmo's second anchor is where the
    // simulated one will start: on anchor A in the authored pose.
    Transform anchor_b = anchor_a;
    const u32 target =
        joint->target == 0 ? ser::WorldNode::kNoParent : world.index_of(joint->target);
    if (target != ser::WorldNode::kNoParent) {
        const Transform body_b = unscaled_placement(world, world.nodes()[target]);
        anchor_b = body_b * gameplay::frame_on_target(body_a, description.frame_a, &body_b);
        // The connection is drawn to body B's origin as well, so which body is joined is visible
        // even though the two anchors coincide.
        sink.line(anchor_a.translation, body_b.translation, physics::DebugColor::Constraint);
    }
    physics::debug_draw_constraint(description, anchor_a, anchor_b, sink);
    if (has_axis(description.type)) {
        sink.line(anchor_a.translation, anchor_a.translation + (anchor_a.right() * kAxisLength),
                  physics::DebugColor::ConstraintLimit);
    }
    return true;
}

}  // namespace cy::sample::editor_window
